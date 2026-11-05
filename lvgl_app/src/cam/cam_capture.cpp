/*
 * cam_capture.cpp —— CamCapture 的实现
 *
 * 【和原来 cam.c 的结构差异】
 *
 *   原来：
 *     static struct {...} g;          ← 全局状态
 *     cam_start() 里 4 条失败路径，每条手工清一批资源
 *     cam_start_cleanup() 补漏（补的还是漏了一条）
 *     cam_stop() 里再清一遍
 *
 *   现在：
 *     所有资源都是成员对象（UniqueFd / MappedBuffer / vector / thread）
 *     start() 里任何一步失败 → 直接 return → 调用方看到错误码
 *     对象析构（或 stop()）→ 统一走 teardown() → 逆序自动释放
 *
 *   "失败路径不用写清理代码"是这次重写最主要的目的。
 */

#include "cam_capture.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#ifdef CAM_USE_RKAIQ
#include "rk_aiq_user_api2_sysctl.h"
#endif

namespace camp {

/* ================================================================== */
/* 日志                                                               */
/* ================================================================== */
void log(const char *fmt, ...)
{
	va_list ap;

	std::printf("[APP ] ");
	va_start(ap, fmt);
	std::vprintf(fmt, ap);
	va_end(ap);
	std::printf("\n");
	std::fflush(stdout);
}

/* ================================================================== */
/* MappedBuffer                                                       */
/* ================================================================== */
bool MappedBuffer::map(int fd, size_t length, off_t offset)
{
	void *p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, offset);

	if (p == MAP_FAILED)
		return false;

	ptr_ = p;
	len_ = length;
	return true;
}

void MappedBuffer::reset()
{
	/* ★ munmap 必须传当初 mmap 的长度，所以长度和指针必须一起存 */
	if (ptr_ && len_)
		::munmap(ptr_, len_);
	ptr_ = nullptr;
	len_ = 0;
}

/* ================================================================== */
/* 小工具                                                             */
/* ================================================================== */

int CamCapture::xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	/* ioctl 可能被信号打断，EINTR 要重试 */
	do {
		r = ::ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

void CamCapture::set_error(const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	{
		std::lock_guard<std::mutex> lk(err_mutex_);
		err_ = buf;
	}
	log("摄像头错误：%s", buf);
}

/* V4L2 多平面：rkisp 的节点是 VIDEO_CAPTURE_MPLANE，用单平面 API 会 EINVAL */
static constexpr v4l2_buf_type kCapType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

static uint64_t monotonic_ms()
{
	timespec ts{};

	::clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000 +
	       static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

/* ================================================================== */
/* 生命周期                                                           */
/* ================================================================== */

CamCapture::~CamCapture()
{
	/* 析构必须 stop，否则线程还在跑时成员先被销毁 → 野指针 */
	stop();
}

int CamCapture::start(const std::string &video_dev,
		      const std::string &iq_dir,
		      bool use_aiq)
{
	if (run_.load(std::memory_order_acquire))
		return 0;                       /* 已经在跑 */

	if (!alloc_preview())
		return -ENOMEM;                 /* 失败：析构时 pv_ 自动释放 */

	if (!open_device(video_dev)) {
		teardown();                     /* 失败也走统一释放，不留半成品 */
		return -EIO;
	}

	/*
	 * ★ rkaiq 必须在 STREAMON【之前】起。
	 *   它 prepare 阶段要把初始 ISP 参数（曝光/增益/矩阵）写进 rkisp。
	 *   等流开始再起的话，前几帧按 ISP 默认参数出图（通常很暗），
	 *   虽然之后会收敛，但录像头几秒就是废的。
	 */
	if (use_aiq)
		aiq_start(video_dev, iq_dir);

	if (!stream_on()) {
		teardown();                     /* 会连带 aiq_stop_locked() */
		return -EIO;
	}
	stream_on_ = true;

	run_.store(true, std::memory_order_release);
	th_ = std::thread(&CamCapture::capture_loop, this);
	if (!th_.joinable()) {
		set_error("采集线程创建失败");
		run_.store(false);
		teardown();
		return -EAGAIN;
	}

	status_ = std::to_string(cap_w_) + "x" + std::to_string(cap_h_);
	if (capture_fps_ > 0)
		status_ += "@" + std::to_string(capture_fps_);
	status_ += aiq_ok_ ? " 3A on" : " 3A off";
	log("摄像头已启动：%s", status_.c_str());
	return 0;
}

void CamCapture::stop()
{
	if (!run_.load(std::memory_order_acquire) && !fd_.valid())
		return;                         /* 本来就停着 */

	run_.store(false, std::memory_order_release);

	if (th_.joinable())
		th_.join();                     /* 等采集线程退出，之后才能安全释放 */

	if (rec_on_.load())
		record_stop();

	teardown();
	log("摄像头已停止");
}

/*
 * teardown —— 唯一的资源释放点。
 *
 * 顺序有讲究，不能随便调：
 *   ① 先停流（否则内核还在往 buffer 里写，munmap 后就是 UAF）
 *   ② 再停 rkaiq（ctx 里挂着它自己的线程，会在 rkisp 参数节点上做事）
 *   ③ 最后解 mmap / 关 fd
 * pv_ 是纯内存，什么时候清都行，交给 vector 自己处理。
 */
void CamCapture::teardown()
{
	if (stream_on_) {
		v4l2_buf_type type = kCapType;

		if (fd_.valid())
			(void)xioctl(fd_.get(), VIDIOC_STREAMOFF, &type);
		stream_on_ = false;
	}

	aiq_stop_locked();

	bufs_.clear();                      /* MappedBuffer 析构 → munmap */
	fd_.reset();                        /* UniqueFd 析构 → close */

	status_.clear();
	aiq_ok_ = false;
}

bool CamCapture::alloc_preview()
{
	const size_t bytes = static_cast<size_t>(kPvW) * kPvH;

	/* vector 管内存，失败就是 bad_alloc 捕获不到的场景 —— 用 try-less 方式判 */
	try {
		pv_[0].assign(bytes, 0);
		pv_[1].assign(bytes, 0);
	} catch (...) {
		set_error("预览缓冲分配失败（内存不足）");
		return false;
	}
	pv_idx_ = 0;
	seq_.store(0);
	return true;
}

/*
 * 开/关整幅 RGB888 通路。
 * 打开时顺手把缓冲按当前采集尺寸分配好；关闭时【不释放】——
 * YOLO 页面来回进出很频繁（点进去看两眼又退），反复 malloc 230KB
 * 只会把堆搞碎，buffer 留着更划算。
 */
void CamCapture::enable_rgb(bool on)
{
	if (on && rgb_.empty()) {
		try {
			rgb_.assign(static_cast<size_t>(cap_w_) * cap_h_ * 3, 0);
		} catch (...) {
			set_error("YOLO 用 RGB 缓冲分配失败（内存不足）");
			rgb_on_.store(false);
			return;
		}
	}
	rgb_on_.store(on, std::memory_order_release);
}

uint32_t CamCapture::frame_rgb(uint8_t *dst, int dst_bytes)
{
	constexpr int need = kCapW * kCapH * 3;

	if (!dst || dst_bytes < need)
		return 0;

	if (!rgb_on_.load(std::memory_order_acquire) || rgb_.empty())
		return 0;

	std::lock_guard<std::mutex> lk(rgb_mutex_);

	if (rgb_seq_ == 0)
		return 0;

	std::memcpy(dst, rgb_.data(), need);
	return rgb_seq_;
}

bool CamCapture::open_device(const std::string &dev)
{
	v4l2_capability cap{};
	v4l2_format fmt{};
	v4l2_streamparm parm{};

	fd_.reset(::open(dev.c_str(), O_RDWR | O_NONBLOCK, 0));
	if (!fd_.valid()) {
		set_error("打开 %s 失败: %s", dev.c_str(), std::strerror(errno));
		return false;
	}

	if (xioctl(fd_.get(), VIDIOC_QUERYCAP, &cap) < 0) {
		set_error("VIDIOC_QUERYCAP 失败: %s", std::strerror(errno));
		return false;
	}
	log("摄像头节点 %s: driver=%s card=%s",
	    dev.c_str(), cap.driver, cap.card);

	/*
	 * 请求 NV12：rkisp mainpath 的原生格式（YUV420SP），
	 * 走这条路径不需要额外转换，也能吃到 ISP 的降噪/锐化。
	 */
	fmt.type = kCapType;
	fmt.fmt.pix_mp.width = kCapW;
	fmt.fmt.pix_mp.height = kCapH;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

	if (xioctl(fd_.get(), VIDIOC_S_FMT, &fmt) < 0) {
		set_error("VIDIOC_S_FMT 失败: %s", std::strerror(errno));
		return false;
	}

	/* 驱动可能改写尺寸/格式，以它返回的为准 */
	cap_w_ = static_cast<int>(fmt.fmt.pix_mp.width);
	cap_h_ = static_cast<int>(fmt.fmt.pix_mp.height);

	const char *fcc = reinterpret_cast<const char *>(&fmt.fmt.pix_mp.pixelformat);
	const bool is_nv12 = (fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12);

	log("plane 数 = %u（NV12 应为 1）", fmt.fmt.pix_mp.num_planes);
	log("采集格式：%dx%d fourcc=%c%c%c%c%s",
	    cap_w_, cap_h_, fcc[0], fcc[1], fcc[2], fcc[3],
	    is_nv12 ? " (NV12 ✓)" : " (非 NV12！转换会错)");

	if (!is_nv12) {
		set_error("驱动没接受 NV12，拿到的是 %c%c%c%c",
			  fcc[0], fcc[1], fcc[2], fcc[3]);
		return false;
	}

	/*
	 * 不写死帧率：枚举当前分辨率/格式支持的档位，选择最短帧间隔，
	 * 也就是摄像头和 ISP 对外声明的最高帧率。驱动不支持枚举时不下发
	 * S_PARM，保留它自己的默认（通常就是 sensor 当前模式的最高速率）。
	 */
	v4l2_fract fastest{};
	bool have_fastest = false;
	for (uint32_t i = 0; ; i++) {
		v4l2_frmivalenum fi{};

		fi.index = i;
		fi.pixel_format = fmt.fmt.pix_mp.pixelformat;
		fi.width = static_cast<uint32_t>(cap_w_);
		fi.height = static_cast<uint32_t>(cap_h_);
		if (xioctl(fd_.get(), VIDIOC_ENUM_FRAMEINTERVALS, &fi) < 0)
			break;

		v4l2_fract candidate{};
		if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE)
			candidate = fi.discrete;
		else if (fi.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
			 fi.type == V4L2_FRMIVAL_TYPE_STEPWISE)
			candidate = fi.stepwise.min;
		else
			continue;

		if (!candidate.numerator || !candidate.denominator)
			continue;
		if (!have_fastest ||
		    static_cast<uint64_t>(candidate.denominator) * fastest.numerator >
		    static_cast<uint64_t>(fastest.denominator) * candidate.numerator) {
			fastest = candidate;
			have_fastest = true;
		}

		/* 连续/步进类型用一个范围描述全部档位，只需读 index 0。 */
		if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE)
			break;
	}

	if (have_fastest) {
		parm = {};
		parm.type = kCapType;
		if (xioctl(fd_.get(), VIDIOC_G_PARM, &parm) == 0) {
			parm.parm.capture.timeperframe = fastest;
			if (xioctl(fd_.get(), VIDIOC_S_PARM, &parm) < 0)
				log("驱动拒绝最高帧率请求，使用默认档位: %s",
				    std::strerror(errno));
		}
	}

	/* 再读一次驱动最终接受的档位，仅用于状态和录像上限。 */
	parm = {};
	parm.type = kCapType;
	capture_fps_ = 0;
	if (xioctl(fd_.get(), VIDIOC_G_PARM, &parm) == 0 &&
	    parm.parm.capture.timeperframe.numerator &&
	    parm.parm.capture.timeperframe.denominator) {
		const v4l2_fract &tpf = parm.parm.capture.timeperframe;

		capture_fps_ = static_cast<int>((tpf.denominator +
					      tpf.numerator / 2) / tpf.numerator);
		log("摄像头帧率档位：%u/%u = %.2f fps%s",
		    tpf.denominator, tpf.numerator,
		    static_cast<double>(tpf.denominator) / tpf.numerator,
		    have_fastest ? "（已选择最高档）" : "（驱动默认）");
	} else {
		log("驱动未报告帧率档位，保持默认并按实际帧计数显示");
	}

	/* ---- 申请 buffer ---- */
	v4l2_requestbuffers req{};

	req.count = kNumBuf;
	req.type = kCapType;
	req.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd_.get(), VIDIOC_REQBUFS, &req) < 0) {
		set_error("VIDIOC_REQBUFS 失败: %s", std::strerror(errno));
		return false;
	}
	if (req.count < 2) {
		set_error("buffer 太少（%u）", req.count);
		return false;
	}

	const unsigned int n = (req.count < kNumBuf) ? req.count : kNumBuf;

	/*
	 * ★ 先 resize 再逐个 map：MappedBuffer 是 move-only，
	 *   某个 map 失败时，前几个已经 map 成功的会在 bufs_ 析构时
	 *   自动 munmap —— 这就是 RAII 在这里的意义。
	 */
	bufs_.resize(n);

	for (unsigned int i = 0; i < n; i++) {
		v4l2_buffer b{};
		v4l2_plane planes[VIDEO_MAX_PLANES]{};

		b.type = kCapType;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		/* mplane 下 b.length 是"planes 数组长度"，不是数据长度 */
		b.length = fmt.fmt.pix_mp.num_planes;
		b.m.planes = planes;

		if (xioctl(fd_.get(), VIDIOC_QUERYBUF, &b) < 0) {
			set_error("VIDIOC_QUERYBUF(%u) 失败: %s", i, std::strerror(errno));
			return false;
		}

		/* 真正的大小/偏移在 planes[0] 里 */
		if (!bufs_[i].map(fd_.get(), planes[0].length,
				  static_cast<off_t>(planes[0].m.mem_offset))) {
			set_error("mmap(%u) 失败: %s", i, std::strerror(errno));
			return false;
		}
		log("buffer %u: %u 字节", i, planes[0].length);
	}

	/* 全部入队，等 STREAMON */
	for (unsigned int i = 0; i < n; i++) {
		v4l2_buffer b{};
		v4l2_plane planes[VIDEO_MAX_PLANES]{};

		b.type = kCapType;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = fmt.fmt.pix_mp.num_planes;
		b.m.planes = planes;

		if (xioctl(fd_.get(), VIDIOC_QBUF, &b) < 0) {
			set_error("首次 QBUF(%u) 失败: %s", i, std::strerror(errno));
			return false;
		}
	}

	return true;
}

bool CamCapture::stream_on()
{
	v4l2_buf_type type = kCapType;

	if (xioctl(fd_.get(), VIDIOC_STREAMON, &type) < 0) {
		set_error("VIDIOC_STREAMON 失败: %s", std::strerror(errno));
		return false;
	}
	return true;
}

/* ================================================================== */
/* rkaiq 3A                                                           */
/* ================================================================== */
#ifdef CAM_USE_RKAIQ

/*
 * 内存快照。
 *
 * 这块板只有 54MB，带 rkaiq 跑相机页整板卡死过。到底哪一步吃掉的
 * 必须能量化：prepare 会起十来个算法线程（AE/AWB/ANR/ASHARP…），
 * 是最可疑的一步，所以 init / prepare 前后各打一次。
 */
static void mem_report(const char *tag)
{
	FILE *f = std::fopen("/proc/meminfo", "r");
	char line[128];
	long total = 0, avail = 0, freeb = 0;

	if (!f)
		return;
	while (std::fgets(line, sizeof(line), f)) {
		if (!std::strncmp(line, "MemTotal:", 9))
			std::sscanf(line + 9, "%ld", &total);
		else if (!std::strncmp(line, "MemAvailable:", 13))
			std::sscanf(line + 13, "%ld", &avail);
		else if (!std::strncmp(line, "MemFree:", 8))
			std::sscanf(line + 8, "%ld", &freeb);
	}
	std::fclose(f);
	log("MEM[%s] total=%ldM free=%ldM avail=%ldM",
	    tag, total / 1024, freeb / 1024, avail / 1024);
}

bool CamCapture::aiq_start(const std::string &video_dev,
			   const std::string &iq_dir)
{
	/*
	 * ★★ 拿 sensor 实体名【必须用 rkaiq 自己的 API，不要自己拼】。
	 *
	 *   rkaiq 内部按固定位置解析这个字符串：
	 *       sscanf(&sns_entity_name[6], "%s", ctx->sns_name);
	 *   —— 假设前 6 个字符是 "m00_b_" 这种前缀。
	 *   名字格式不对 → sysctl_init 返回 NULL，日志只有一句
	 *   "MessageParser process loop exit!"，什么原因都不说。
	 *
	 *   正确做法：getBindedSnsEntNmByVd() 顺着 video 节点在 media 拓扑里
	 *   反查真正绑定的 sensor 实体名，换模组也不用改代码。
	 */
	const char *n = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(video_dev.c_str());
	std::string entity;

	if (!n || !n[0]) {
		log("rkaiq: getBindedSnsEntNmByVd(%s) 没拿到实体名，尝试兜底",
		    video_dev.c_str());
		entity = "m00_b_sc3336 4-0030";     /* 板上实测名，仅作最后尝试 */
	} else {
		entity = n;
	}

	log("rkaiq: sensor 实体 = \"%s\"，iq 目录 = %s",
	    entity.c_str(), iq_dir.c_str());

	/*
	 * ★ init 之前必须做两个 preInit（照 rkisp_demo.cpp 抄）：
	 *   preInit_tb_info：tuning base 信息。magic 必须是 sizeof-2，
	 *                    改成 sizeof 会被判非法而忽略。
	 *   preInit_scene  ：决定加载 iq 里哪一段参数，不设则内部状态
	 *                    未初始化，init 必然失败。
	 */
	{
		rk_aiq_tb_info_t tb{};

		tb.magic = sizeof(rk_aiq_tb_info_t) - 2;
		tb.is_pre_aiq = false;
		rk_aiq_uapi2_sysctl_preInit_tb_info(entity.c_str(), &tb);
	}

	if (rk_aiq_uapi2_sysctl_preInit_scene(entity.c_str(), "normal", "day") !=
	    XCAM_RETURN_NO_ERROR)
		log("rkaiq: preInit_scene 失败（scene 名可能不被这支 iq 支持，继续）");

	mem_report("rkaiq init 前");

	auto *ctx = rk_aiq_uapi2_sysctl_init(entity.c_str(), iq_dir.c_str(),
					     nullptr, nullptr);
	if (!ctx) {
		set_error("rkaiq init 失败（实体名或 iq 目录不对）");
		return false;
	}
	aiq_ctx_ = ctx;
	log("rkaiq: init 成功，开始 prepare %dx%d", cap_w_, cap_h_);
	mem_report("init 后");

	if (rk_aiq_uapi2_sysctl_prepare(ctx, cap_w_, cap_h_,
					RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR) {
		set_error("rkaiq prepare 失败");
		aiq_stop_locked();              /* ctx 十几 MB，立刻还回去 */
		return false;
	}

	if (rk_aiq_uapi2_sysctl_start(ctx) != XCAM_RETURN_NO_ERROR) {
		set_error("rkaiq start 失败");
		aiq_stop_locked();
		return false;
	}

	aiq_ok_ = true;
	mem_report("prepare+start 后");
	log("rkaiq: 3A 已启动（自动曝光/白平衡生效）");
	return true;
}

void CamCapture::aiq_stop_locked()
{
	auto *ctx = static_cast<rk_aiq_sys_ctx_t *>(aiq_ctx_);

	if (ctx) {
		rk_aiq_uapi2_sysctl_stop(ctx, false);
		rk_aiq_uapi2_sysctl_deinit(ctx);
		aiq_ctx_ = nullptr;
	}
	aiq_ok_ = false;
}

#else  /* 未编入 rkaiq */

bool CamCapture::aiq_start(const std::string &, const std::string &)
{
	set_error("编译时未启用 rkaiq（CAM_USE_RKAIQ），图像可能偏暗");
	return false;
}
void CamCapture::aiq_stop_locked() { }

#endif /* CAM_USE_RKAIQ */

/* ================================================================== */
/* 采集线程                                                           */
/* ================================================================== */

void CamCapture::capture_loop()
{
	while (run_.load(std::memory_order_acquire)) {
		pollfd pfd{};

		pfd.fd = fd_.get();
		pfd.events = POLLIN;

		/*
		 * ★ 用 poll 等帧，不要死循环 DQBUF。
		 *   没帧时 DQBUF 返回 EAGAIN，纯 while 会吃满一个核，
		 *   把单核板子上的 LVGL 和音频线程都饿死。
		 */
		if (::poll(&pfd, 1, 1000) <= 0)
			continue;

		v4l2_buffer b{};
		v4l2_plane planes[VIDEO_MAX_PLANES]{};

		b.type = kCapType;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = VIDEO_MAX_PLANES;
		b.m.planes = planes;

		if (xioctl(fd_.get(), VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			if (run_.load())
				set_error("DQBUF 失败: %s", std::strerror(errno));
			break;
		}

		if (b.index < bufs_.size() && planes[0].bytesused > 0) {
			uint8_t *base = bufs_[b.index].data();

			/* NV12：Y 平面在前 (w*h)，UV 在后 (w*h/2) */
			handle_frame(base, base + static_cast<size_t>(cap_w_) * cap_h_);
		}

		if (xioctl(fd_.get(), VIDIOC_QBUF, &b) < 0) {
			if (run_.load())
				set_error("QBUF 失败: %s", std::strerror(errno));
			break;
		}
	}
}

void CamCapture::handle_frame(const uint8_t *y, const uint8_t *uv)
{
	/* ---- 预览：写后备缓冲，写完再切换并 bump 序号 ---- */
	{
		std::lock_guard<std::mutex> lk(pv_mutex_);

		nv12_to_rgb565(y, uv, cap_w_, cap_h_,
			       pv_[pv_idx_ ^ 1].data(), kPvW, kPvH);
		pv_idx_ ^= 1;
		seq_.fetch_add(1, std::memory_order_release);
	}

	/* ---- YOLO 用的整幅 RGB888 ---- */
	if (rgb_on_.load(std::memory_order_acquire) && !rgb_.empty()) {
		std::lock_guard<std::mutex> lk(rgb_mutex_);

		nv12_to_rgb24(y, uv, cap_w_, cap_h_,
			      rgb_.data(), cap_w_, cap_h_);
		rgb_seq_++;
	}

	/* ---- 拍照 ---- */
	if (photo_req_.load(std::memory_order_acquire)) {
		std::string path;

		{
			std::lock_guard<std::mutex> lk(photo_mutex_);
			path = photo_path_;
		}

		std::vector<uint8_t> bgr(static_cast<size_t>(cap_w_) * cap_h_ * 3);

		nv12_to_bgr24(y, uv, cap_w_, cap_h_, bgr.data(), cap_w_, cap_h_);
		if (bmp_write(path.c_str(), bgr.data(), cap_w_, cap_h_) == 0) {
			{
				std::lock_guard<std::mutex> lk(photo_mutex_);
				photo_last_ = path;
			}
			photo_cnt_.fetch_add(1);
			log("拍照已保存：%s", path.c_str());
		} else {
			set_error("写 BMP 失败：%s", path.c_str());
		}
		photo_req_.store(false, std::memory_order_release);
	}

	/* ---- 录像：按 fps 抽帧 ---- */
	if (rec_on_.load(std::memory_order_acquire)) {
		const uint64_t now = monotonic_ms();
		const uint64_t period = 1000 / static_cast<uint64_t>(rec_fps_);

		if (now >= rec_next_frame_ms_) {
			std::vector<uint8_t> bgr(static_cast<size_t>(kPvW) * kPvH * 3);

			nv12_to_bgr24(y, uv, cap_w_, cap_h_,
				      bgr.data(), kPvW, kPvH);
			avi_add_frame(&rec_w_, bgr.data());
			rec_next_frame_ms_ += period ? period : 1;
			if (rec_next_frame_ms_ <= now)
				rec_next_frame_ms_ = now + (period ? period : 1);
		}

		/* 用单调时钟计时，不再依赖任何写死的摄像头帧率。 */
		const int sec = static_cast<int>((now - rec_started_ms_) / 1000);

		if (sec != rec_sec_.load()) {
			rec_sec_.store(sec);
			if (sec >= kRecMaxSec) {
				log("录像达到 %d 秒上限，自动停止", kRecMaxSec);
				record_stop();
			}
		}
	}
}

/* ================================================================== */
/* 预览 / 拍照 / 录像 的对外方法                                       */
/* ================================================================== */

uint32_t CamCapture::preview(uint16_t *dst, int dst_bytes)
{
	/*
	 * ★★★ 越界校验 —— 整个模块最重要的一行防御。
	 *
	 *   历史上这里没有校验，调用方 canvas 是 320x160（100KB），
	 *   而本函数按 CAM_PV_H=240 拷（150KB）→ 每帧越界写 50KB，
	 *   踩坏 LVGL 的对象内存。症状：画面下半部分乱、跑十几秒后
	 *   进程无任何报错就消失。没有编译期提示，只能靠显式校验挡住。
	 */
	constexpr int need = kPvW * kPvH * 2;

	if (!dst || dst_bytes < need)
		return 0;

	const uint32_t s = seq_.load(std::memory_order_acquire);

	if (s == 0 || !run_.load(std::memory_order_acquire))
		return 0;

	std::lock_guard<std::mutex> lk(pv_mutex_);

	/* 读"已经写完"的那个：pv_idx_ 指向下次要写的位置，所以可读的是 ^1 */
	std::memcpy(dst, pv_[pv_idx_ ^ 1].data(), need);
	return s;
}

int CamCapture::photo(const std::string &path)
{
	if (!run_.load(std::memory_order_acquire) || path.empty())
		return -1;
	if (photo_req_.load(std::memory_order_acquire))
		return -1;                      /* 上一张还没写完 */

	{
		std::lock_guard<std::mutex> lk(photo_mutex_);
		photo_path_ = path;
	}
	photo_req_.store(true, std::memory_order_release);
	return 0;
}

std::string CamCapture::last_photo() const
{
	std::lock_guard<std::mutex> lk(photo_mutex_);
	return photo_last_;
}

int CamCapture::record_start(const std::string &path, int fps)
{
	if (!run_.load(std::memory_order_acquire) || rec_on_.load())
		return -1;
	if (fps < 1)
		fps = 8;
	if (capture_fps_ > 0 && fps > capture_fps_)
		fps = capture_fps_;

	if (avi_open(&rec_w_, path.c_str(), kPvW, kPvH, fps) != 0) {
		set_error("创建录像文件失败：%s", path.c_str());
		return -1;
	}

	rec_fps_ = fps;
	rec_started_ms_ = monotonic_ms();
	rec_next_frame_ms_ = rec_started_ms_;
	rec_sec_.store(0);
	rec_on_.store(true, std::memory_order_release);
	log("开始录像：%s  %dx%d@%dfps（上限 %d 秒）",
	    path.c_str(), kPvW, kPvH, fps, kRecMaxSec);
	return 0;
}

int CamCapture::record_stop()
{
	if (!rec_on_.load(std::memory_order_acquire))
		return -1;

	rec_on_.store(false, std::memory_order_release);
	avi_close(&rec_w_);

	const int frames = static_cast<int>(rec_w_.frames);
	const int elapsed = rec_started_ms_ ?
		static_cast<int>((monotonic_ms() - rec_started_ms_) / 1000) : 0;

	log("录像结束：%d 帧 / %d 秒", frames, rec_sec_.load());
	rec_sec_.store(elapsed);
	return 0;
}

std::string CamCapture::status() const { return status_; }

std::string CamCapture::last_error() const
{
	std::lock_guard<std::mutex> lk(err_mutex_);
	return err_;
}

/* ================================================================== */
/* 单例 + C 接口（给 ui_camera.c 用，行为与旧版完全一致）              */
/* ================================================================== */

CamCapture &instance()
{
	/*
	 * 函数内 static：C++11 起保证线程安全初始化。
	 * 析构时机由 atexit 决定，进程退出时自动 stop()。
	 */
	static CamCapture g;
	return g;
}

} /* namespace camp */

/* ------------------------------------------------------------------ */
/* extern "C" —— 保持 cam.h 的接口不变，ui_camera.c 一行都不用改       */
/* ------------------------------------------------------------------ */
extern "C" {

int cam_start(const char *video_dev, const char *iq_dir)
{
	const char *off = std::getenv("CAM_AIQ");
	const bool use_aiq = !(off && std::strcmp(off, "off") == 0);

	if (!use_aiq)
		camp::log("rkaiq: CAM_AIQ=off，跳过 3A（画面会偏暗，仅调试用）");

	return camp::instance().start(video_dev ? video_dev : "/dev/video0",
				      iq_dir ? iq_dir : "/userdata/cam/iqfiles",
				      use_aiq);
}

void cam_stop(void) { camp::instance().stop(); }

bool cam_running(void) { return camp::instance().running(); }

uint32_t cam_preview(uint16_t *dst, int dst_bytes)
{
	return camp::instance().preview(dst, dst_bytes);
}

/* YOLO 用的整幅 RGB888：进页面时开，退页面时关（省 CPU） */
void cam_rgb_enable(int on) { camp::instance().enable_rgb(on != 0); }

uint32_t cam_frame_rgb(uint8_t *dst, int dst_bytes)
{
	return camp::instance().frame_rgb(dst, dst_bytes);
}

int cam_capture_size(int *w, int *h)
{
	if (w)
		*w = camp::instance().cap_w();
	if (h)
		*h = camp::instance().cap_h();
	return 0;
}

int cam_photo(const char *path)
{
	return path ? camp::instance().photo(path) : -1;
}

uint32_t cam_photo_count(void) { return camp::instance().photo_count(); }

/*
 * ★ 返回 const char* 给 C 用，但内部是 std::string。
 *   放进一个 thread_local 缓冲里拷出来 —— 每次调用覆盖上一次的内容，
 *   和旧版"返回静态数组"的行为一致（调用方也只是马上 snprintf 走）。
 */
const char *cam_last_photo(void)
{
	static thread_local char buf[128];

	std::snprintf(buf, sizeof(buf), "%s", camp::instance().last_photo().c_str());
	return buf;
}

int cam_record_start(const char *path, int fps)
{
	return path ? camp::instance().record_start(path, fps) : -1;
}

int cam_record_stop(void) { return camp::instance().record_stop(); }

bool cam_recording(void) { return camp::instance().recording(); }

int cam_record_seconds(void) { return camp::instance().record_seconds(); }

const char *cam_status(void)
{
	static thread_local char buf[128];

	std::snprintf(buf, sizeof(buf), "%s", camp::instance().status().c_str());
	return buf;
}

const char *cam_last_error(void)
{
	static thread_local char buf[256];

	std::snprintf(buf, sizeof(buf), "%s", camp::instance().last_error().c_str());
	return buf;
}

} /* extern "C" */
