/*
 * cam_capture.hpp —— V4L2 采集 + rkaiq 3A 的 C++ 封装
 *
 * 【为什么改成 C++】
 *   原来 cam.c 是一个 static 全局结构体 + 一堆静态函数，用起来有两个硬伤：
 *
 *   ① 资源释放全靠手写，漏一条路径就泄漏。
 *      实际踩过：cam_start() 在 v4l2_init() 失败时直接 return -1，
 *      已经 malloc 的两块预览缓冲没 free，指针又被下次的 memset(&g,0)
 *      覆盖 → 永久泄漏。UI 每 20ms 重试一次 → 169 次 × 200KB = 34MB
 *      → 内核 OOM 杀掉整个进程，界面上却只显示"打开失败"。
 *
 *      这个 bug 的本质不是"忘了写 free"，而是【失败路径有 N 条，
 *      每条都要记得清理同样一批资源】。C++ 的 RAII 把这件事变成
 *      "只要构造成功就一定有析构"，从结构上消灭这类错误。
 *
 *   ② 全局状态无法同时开两路相机，也不好做单元测试。
 *
 * 【设计取舍】
 *   · **不抛异常**：这块板子跑的是 uClibc，且要和大量 C 代码混编，
 *     异常穿越 C 边界是未定义行为。错误一律用返回码 + last_error()。
 *   · **保留 C 接口**：ui_camera.c 是 C 写的，用它原有的 cam_xxx() 函数。
 *     那些函数现在是本类实例的薄封装（见文件末尾的 extern "C" 段），
 *     UI 一行都不用改，行为完全兼容。
 *   · **单例**：C 接口背后只有一个实例（cam_capture_instance()）。
 *     想开多路的话直接用 CamCapture 类，别走 C 接口。
 */

#ifndef CAM_CAPTURE_HPP
#define CAM_CAPTURE_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>     /* ::close —— UniqueFd 的析构要用 */

#include "cam_image.h"      /* nv12_to_rgb565 / nv12_to_bgr24 / avi_writer（纯 C） */
#include "h265_recorder.hpp"

/*
 * 预览尺寸 ← 统一在这里定义，cam.h 的宏必须和它一致。
 *
 * ★ 历史上 cam.h 的 CAM_PV_H 是 240，而 ui_camera.c 的 canvas 是 320x160，
 *   每帧越界写 50KB，踩坏 LVGL 对象内存 → 画面下半部分异常 + 进程静默消失。
 *   现在两边都引用同一个来源，见 cam.h 里的 static_assert。
 */
constexpr int kPvW = 320;
constexpr int kPvH = 160;

constexpr int kCapW = 640;          /* 拍照与录像使用完整 640×480 帧 */
constexpr int kCapH = 480;
constexpr int kNumBuf = 4;
constexpr int kRecMaxSec = 10;      /* 录像时长上限，防止把 /tmp 写满 */

namespace camp {

/* ------------------------------------------------------------------ */
/* RAII 小工具                                                        */
/* ------------------------------------------------------------------ */

/**
 * 文件描述符。析构自动 close，移动语义支持把所有权交出去。
 *
 * 有了它，"open 成功但后面某步失败忘了 close" 这类问题不可能再出现 ——
 * 因为失败时对象析构，close 自动跑。
 */
class UniqueFd {
public:
	UniqueFd() = default;
	explicit UniqueFd(int fd) : fd_(fd) {}

	~UniqueFd() { reset(); }

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;

	UniqueFd(UniqueFd &&o) noexcept : fd_(o.release()) {}
	UniqueFd &operator=(UniqueFd &&o) noexcept
	{
		if (this != &o) {
			reset();
			fd_ = o.release();
		}
		return *this;
	}

	int get() const { return fd_; }
	bool valid() const { return fd_ >= 0; }

	int release()
	{
		int f = fd_;

		fd_ = -1;
		return f;
	}

	void reset(int fd = -1)
	{
		if (fd_ >= 0)
			::close(fd_);
		fd_ = fd;
	}

private:
	int fd_ = -1;
};

/**
 * mmap 出来的 V4L2 缓冲区。
 *
 * ★ 为什么要单独一个类：munmap 必须传"当初 mmap 的长度"，
 *   所以长度要和指针绑在一起存。原来用 struct cam_buf{void*,size_t}
 *   手工管理，容易在部分成功时漏掉前面几个。
 */
class MappedBuffer {
public:
	MappedBuffer() = default;
	~MappedBuffer() { reset(); }

	MappedBuffer(const MappedBuffer &) = delete;
	MappedBuffer &operator=(const MappedBuffer &) = delete;

	MappedBuffer(MappedBuffer &&o) noexcept
		: ptr_(o.ptr_), len_(o.len_)
	{
		o.ptr_ = nullptr;
		o.len_ = 0;
	}

	MappedBuffer &operator=(MappedBuffer &&o) noexcept
	{
		if (this != &o) {
			reset();
			ptr_ = o.ptr_;
			len_ = o.len_;
			o.ptr_ = nullptr;
			o.len_ = 0;
		}
		return *this;
	}

	bool map(int fd, size_t length, off_t offset);
	void reset();

	uint8_t *data() const { return static_cast<uint8_t *>(ptr_); }
	size_t size() const { return len_; }
	bool valid() const { return ptr_ != nullptr; }

private:
	void *ptr_ = nullptr;
	size_t len_ = 0;
};

/* ------------------------------------------------------------------ */
/* 相机                                                               */
/* ------------------------------------------------------------------ */

class CamCapture {
public:
	CamCapture() = default;
	~CamCapture();

	CamCapture(const CamCapture &) = delete;
	CamCapture &operator=(const CamCapture &) = delete;

	/**
	 * 打开设备、起 3A、起采集线程。
	 *
	 * ★ 失败时对象自动回到"未启动"状态，所有已拿到的资源由析构函数释放。
	 *   调用方【不需要】写任何清理代码 —— 这正是替换掉原 cam_start /
	 *   cam_start_cleanup 那套手工清理的意义。
	 *
	 * @param video_dev  如 "/dev/video11"（rkisp mainpath）
	 * @param iq_dir     iq 参数目录，空则用 /userdata/cam/iqfiles
	 * @param use_aiq    false 则跳过 rkaiq（调试用，图会偏暗）
	 * @return 0 成功；负值为错误码，原因见 last_error()
	 */
	int start(const std::string &video_dev,
		  const std::string &iq_dir = "/userdata/cam/iqfiles",
		  bool use_aiq = true);

	void stop();
	bool running() const { return run_.load(std::memory_order_acquire); }
	int set_capture_size(int width, int height);

	/**
	 * 拷一帧 RGB565 到 dst。
	 * @param dst_bytes dst 的字节数，不够就拒绝拷贝（防越界）
	 * @return 帧序号，>0 表示有新帧；0 表示没有新帧或缓冲太小
	 */
	uint32_t preview(uint16_t *dst, int dst_bytes);

	/*
	 * 整幅 RGB888 通路（YOLO 用）。
	 *   enable_rgb(true)  之后每一帧都会顺带转一份 RGB888
	 *   frame_rgb(dst,bytes) 拷走最新一份，返回帧序号（0 = 还没有）
	 * 尺寸固定 cap_w() x cap_h()（当前 640x480），紧凑排列 R,G,B。
	 */
	void enable_rgb(bool on);
	uint32_t frame_rgb(uint8_t *dst, int dst_bytes);
	int cap_w() const { return cap_w_; }
	int cap_h() const { return cap_h_; }

	int photo(const std::string &path);
	uint32_t photo_count() const { return photo_cnt_.load(); }
	std::string last_photo() const;

	int record_start(const std::string &path, int fps);
	int record_start_h265(const std::string &path);
	int record_stop();
	bool recording() const { return rec_on_.load(); }
	int record_seconds() const { return rec_sec_.load(); }

	std::string status() const;
	std::string last_error() const;

private:
	/* ---- 各阶段（失败就 return，析构负责收尾）---- */
	bool alloc_preview();
	bool open_device(const std::string &dev);
	bool stream_on();
	void teardown();                    /* 唯一一处集中释放 */

	/* ---- rkaiq ---- */
	bool aiq_start(const std::string &video_dev, const std::string &iq_dir);
	void aiq_stop_locked();

	/* ---- 线程 ---- */
	void capture_loop();
	bool handle_frame(uint8_t *y, uint8_t *uv, int dmabuf_fd, size_t bytes);
	bool queue_buffer(unsigned int index);
	void requeue_venc_buffers();

	void set_error(const char *fmt, ...);
	static int xioctl(int fd, unsigned long req, void *arg);

	/* ---- 资源（声明顺序 = 析构逆序，要注意依赖）---- */
	UniqueFd fd_;
	std::vector<MappedBuffer> bufs_;
	std::vector<UniqueFd> dmabuf_fds_;
	std::vector<bool> venc_held_;
	bool stream_on_ = false;

	void *aiq_ctx_ = nullptr;           /* rk_aiq_sys_ctx_t*，避免头文件外泄 */
	bool aiq_ok_ = false;

	std::thread th_;
	std::atomic<bool> run_{ false };

	/* 预览双缓冲 */
	std::vector<uint16_t> pv_[2];
	int pv_idx_ = 0;
	std::atomic<uint32_t> seq_{ 0 };
	mutable std::mutex pv_mutex_;

	/*
	 * 整幅 RGB888 快照（给 YOLO 用）。
	 *
	 * 【为什么不复用 pv_ 的 RGB565】
	 *   预览缓冲是 320x160 的【居中裁剪】；而检测要看整幅 640x480，
	 *   裁掉的部分里有人就直接漏检了。另外 YOLO 要 RGB，不是 RGB565。
	 *
	 * 【为什么默认关闭】
	 *   NV12→RGB888 是 640x480 逐像素算，开着就一直吃 CPU。只有 YOLO
	 *   页面在的时候才需要，所以做成开关，进页面开、退页面关。
	 */
	std::atomic<bool> rgb_on_{ false };
	std::atomic<bool> rgb_requested_{ false };
	std::vector<uint8_t> rgb_;
	uint32_t rgb_seq_ = 0;
	uint32_t rgb_read_seq_ = 0;
	mutable std::mutex rgb_mutex_;

	/* 拍照请求（主线程置位，采集线程执行）*/
	std::atomic<bool> photo_req_{ false };
	mutable std::mutex photo_mutex_;   /* 保护 photo_path_；last_photo() 是 const 也要锁 */
	std::string photo_path_;
	std::string photo_last_;
	std::atomic<uint32_t> photo_cnt_{ 0 };

	/* 录像 */
	std::atomic<bool> rec_on_{ false };
	enum class RecordMode { Avi, H265Mp4 };
	RecordMode rec_mode_ = RecordMode::Avi;
	mutable std::mutex rec_mutex_;
	avi_writer rec_w_{};
	std::vector<uint8_t> rec_bgr_;      /* AVI 640×480 转换缓冲，启动时一次申请 */
	H265Recorder h265_;
	int rec_fps_ = 0;
	uint64_t rec_started_ms_ = 0;
	uint64_t rec_next_frame_ms_ = 0;
	std::atomic<int> rec_sec_{ 0 };

	int cap_w_ = kCapW;
	int cap_h_ = kCapH;
	int requested_w_ = kCapW;
	int requested_h_ = kCapH;
	int capture_fps_ = 0;		/* 驱动确认的实际档位；0 表示由驱动自行决定 */

	mutable std::mutex err_mutex_;
	std::string err_;
	std::string status_;
};

/** C 接口背后用的单例 */
CamCapture &instance();

/* 日志（和 C++ 侧 wake bridge 的格式保持一致） */
void log(const char *fmt, ...);

} /* namespace camp */

#endif /* CAM_CAPTURE_HPP */
