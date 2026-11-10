#include "h265_recorder.hpp"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "resources.hpp"

extern "C" {
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_mb_cmd.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>
}

namespace camp {

static constexpr const char *kCodecDir = "/tmp/cam_codec";
static constexpr const char *kTag = "echomate_zero_copy";

struct H265Recorder::Api {
	MPP_RET (*create)(MppCtx *, MppApi **);
	MPP_RET (*init)(MppCtx, MppCtxType, MppCodingType);
	MPP_RET (*destroy)(MppCtx);
	MPP_RET (*buffer_import)(MppBufferGroup, MppBufferInfo *, MppBuffer *,
				 const char *, const char *);
	MPP_RET (*buffer_put)(MppBuffer, const char *);
	MPP_RET (*frame_init)(MppFrame *);
	MPP_RET (*frame_deinit)(MppFrame *);
	void (*frame_width)(MppFrame, RK_U32);
	void (*frame_height)(MppFrame, RK_U32);
	void (*frame_hstride)(MppFrame, RK_U32);
	void (*frame_vstride)(MppFrame, RK_U32);
	void (*frame_fmt)(MppFrame, MppFrameFormat);
	void (*frame_pts)(MppFrame, RK_S64);
	void (*frame_buffer)(MppFrame, MppBuffer);
	MPP_RET (*cfg_init)(MppEncCfg *);
	MPP_RET (*cfg_deinit)(MppEncCfg);
	MPP_RET (*cfg_s32)(MppEncCfg, const char *, RK_S32);
	MPP_RET (*cfg_u32)(MppEncCfg, const char *, RK_U32);
};

template <typename T>
static bool sym(void *lib, const char *name, T &out)
{
	out = reinterpret_cast<T>(::dlsym(lib, name));
	return out != nullptr;
}

H265Recorder::~H265Recorder()
{
	if (active_.load())
		stop();
	else
		cleanup_encoder();
	delete api_;
	if (mpp_lib_)
		::dlclose(mpp_lib_);
}

int H265Recorder::load_api()
{
	if (api_)
		return 0;

	const std::string path = std::string(kCodecDir) + "/librockchip_mpp.so.1";
	mpp_lib_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
	if (!mpp_lib_) {
		const char *why = ::dlerror();
		error_ = std::string("MPP 运行库未就绪: ") + (why ? why : "dlopen");
		return -1;
	}

	api_ = new Api{};
#define LOAD(name, field) if (!sym(mpp_lib_, name, api_->field)) goto missing
	LOAD("mpp_create", create);
	LOAD("mpp_init", init);
	LOAD("mpp_destroy", destroy);
	LOAD("mpp_buffer_import_with_tag", buffer_import);
	LOAD("mpp_buffer_put_with_caller", buffer_put);
	LOAD("mpp_frame_init", frame_init);
	LOAD("mpp_frame_deinit", frame_deinit);
	LOAD("mpp_frame_set_width", frame_width);
	LOAD("mpp_frame_set_height", frame_height);
	LOAD("mpp_frame_set_hor_stride", frame_hstride);
	LOAD("mpp_frame_set_ver_stride", frame_vstride);
	LOAD("mpp_frame_set_fmt", frame_fmt);
	LOAD("mpp_frame_set_pts", frame_pts);
	LOAD("mpp_frame_set_buffer", frame_buffer);
	LOAD("mpp_enc_cfg_init", cfg_init);
	LOAD("mpp_enc_cfg_deinit", cfg_deinit);
	LOAD("mpp_enc_cfg_set_s32", cfg_s32);
	LOAD("mpp_enc_cfg_set_u32", cfg_u32);
#undef LOAD
	return 0;

missing:
	error_ = "MPP 运行库接口不完整";
	delete api_;
	api_ = nullptr;
	return -1;
}

int H265Recorder::start(const std::string &mp4_path, int width, int height, int fps)
{
	if (active_.load() || mp4_path.empty() || width <= 0 || height <= 0)
		return -1;

	if (::access("/dev/mpi/valloc", R_OK | W_OK) != 0 ||
	    ::access("/dev/mpi/venc", R_OK | W_OK) != 0 ||
	    ::access((std::string(kCodecDir) + "/librockchip_mpp.so.1").c_str(), R_OK) != 0) {
		char output[256];
		const char *const argv[] = { "/root/bin/load_h265.sh", nullptr };
		if (res::run_argv(argv, output, sizeof(output), true) != 0) {
			error_ = std::string("H265 编码环境加载失败: ") + output;
			return -1;
		}
	}
	if (load_api() != 0)
		return -1;

	width_ = width;
	height_ = height;
	fps_ = fps > 0 ? fps : 30;
	mp4_path_ = mp4_path;
	raw_path_ = mp4_path + ".h265";
	frames_.store(0);
	error_.clear();

	raw_file_ = std::fopen(raw_path_.c_str(), "wb");
	if (!raw_file_) {
		error_ = "无法创建 H265 临时文件";
		return -1;
	}
	valloc_fd_ = ::open("/dev/mpi/valloc", O_RDWR | O_CLOEXEC);
	if (valloc_fd_ < 0) {
		error_ = "无法打开 MPP 输出缓冲设备";
		cleanup_encoder();
		return -1;
	}

	MppCtx ctx = nullptr;
	MppApi *mpi = nullptr;
	if (api_->create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
		error_ = "mpp_create 失败";
		cleanup_encoder();
		return -1;
	}
	mpp_ctx_ = ctx;
	mpp_api_ = mpi;

	if (api_->init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
		error_ = "mpp_init H265 失败";
		cleanup_encoder();
		return -1;
	}

	MppEncCfg cfg = nullptr;
	if (api_->cfg_init(&cfg) != MPP_OK ||
	    mpi->control(ctx, MPP_ENC_GET_CFG, cfg) != MPP_OK) {
		error_ = "读取 MPP 编码配置失败";
		cleanup_encoder();
		return -1;
	}
	mpp_cfg_ = cfg;

	const int bps = 512 * 1000;
	api_->cfg_s32(cfg, "prep:width", width_);
	api_->cfg_s32(cfg, "prep:height", height_);
	api_->cfg_s32(cfg, "prep:hor_stride", width_);
	api_->cfg_s32(cfg, "prep:ver_stride", height_);
	api_->cfg_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	api_->cfg_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	api_->cfg_s32(cfg, "rc:fps_in_flex", 0);
	api_->cfg_s32(cfg, "rc:fps_in_num", fps_);
	api_->cfg_s32(cfg, "rc:fps_in_denorm", 1);
	api_->cfg_s32(cfg, "rc:fps_out_flex", 0);
	api_->cfg_s32(cfg, "rc:fps_out_num", fps_);
	api_->cfg_s32(cfg, "rc:fps_out_denorm", 1);
	api_->cfg_s32(cfg, "rc:gop", fps_);
	api_->cfg_s32(cfg, "rc:bps_target", bps);
	api_->cfg_s32(cfg, "rc:bps_max", bps * 17 / 16);
	api_->cfg_s32(cfg, "rc:bps_min", bps * 15 / 16);
	api_->cfg_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
	api_->cfg_s32(cfg, "rc:qp_init", 26);
	api_->cfg_s32(cfg, "rc:qp_max", 51);
	api_->cfg_s32(cfg, "rc:qp_min", 10);
	api_->cfg_s32(cfg, "rc:qp_max_i", 51);
	api_->cfg_s32(cfg, "rc:qp_min_i", 10);
	api_->cfg_s32(cfg, "rc:qp_ip", 2);
	api_->cfg_s32(cfg, "codec:type", MPP_VIDEO_CodingHEVC);

	if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
		error_ = "设置 MPP H265 参数失败";
		cleanup_encoder();
		return -1;
	}
	MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
	mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);

	active_.store(true);
	return 0;
}

int H265Recorder::submit_dmabuf(int fd, uint8_t *addr, size_t bytes,
				uint64_t pts_us)
{
	const size_t need = static_cast<size_t>(width_) * height_ * 3 / 2;
	if (!active_.load() || fd < 0 || !addr || bytes < need)
		return -1;

	const int fno = frames_.load();

	auto *mpi = static_cast<MppApi *>(mpp_api_);
	auto ctx = static_cast<MppCtx>(mpp_ctx_);
	/*
	 * ★★★ 根因（2026-11-10 实测）：mpp_buffer_import_with_tag() 会接管
	 * info.fd 的所有权，buffer_put() 时把它 close 掉。直接传 V4L2 的
	 * DMA-BUF fd，等于每帧关掉一个 V4L2 buffer 的 fd —— 4 帧后
	 * fd 8/9/10/11 全关、ISP 不再出帧、采集线程永远卡在 poll()。
	 * 只把 dup 出来的副本交给 MPP；dup 指向同一块 dma_buf（同一物理
	 * 内存），仍然是零拷贝，没有引入任何帧拷贝。
	 */
	const int import_fd = ::dup(fd);
	if (import_fd < 0) {
		error_ = "dup 摄像头 DMA-BUF fd 失败";
		return -1;
	}

	MppBufferInfo info{};
	info.type = MPP_BUFFER_TYPE_ION;
	info.size = bytes;
	info.ptr = addr;
	info.fd = import_fd;
	info.index = -1;

	MppBuffer buffer = nullptr;
	MPP_RET rc = api_->buffer_import(nullptr, &info, &buffer, kTag,
					 "submit_dmabuf");
	if (rc != MPP_OK || !buffer) {
		::close(import_fd);	/* MPP 未接管，副本自己收 */
		std::fprintf(stderr,
			     "[APP ] H265 零拷贝：导入 DMA-BUF 失败 rc=%d\n", rc);
		error_ = "MPP 导入摄像头 DMA-BUF 失败";
		return -1;
	}

	MppFrame frame = nullptr;
	rc = api_->frame_init(&frame);
	if (rc == MPP_OK && frame) {
		api_->frame_width(frame, width_);
		api_->frame_height(frame, height_);
		api_->frame_hstride(frame, width_);
		api_->frame_vstride(frame, height_);
		api_->frame_fmt(frame, MPP_FMT_YUV420SP);
		api_->frame_pts(frame, static_cast<RK_S64>(pts_us));
		api_->frame_buffer(frame, buffer);
		rc = mpi->encode_put_frame(ctx, frame);
		api_->frame_deinit(&frame);
	} else {
		rc = MPP_NOK;
	}
	/*
	 * put_frame 返回后即可 put 输入 buffer。SDK 把 encode_put_frame 标注为
	 * async 接口，但 RV1106 这版 MPP 实际是阻塞输入模式：返回时 VENC 已读完
	 * 像素。板端实测 63 帧连续录制无花屏/撕裂，且 buffer_put 后 V4L2 原 fd
	 * 仍 alive（配合上面的 dup），据此确认可立即归还。
	 */
	api_->buffer_put(buffer, "submit_dmabuf");
	if (rc != MPP_OK) {
		char msg[96];
		std::snprintf(msg, sizeof(msg), "MPP 提交 DMA-BUF 失败: rc=%d", rc);
		error_ = msg;
		return -1;
	}

	struct venc_packet packet_data{};
	MppPacket packet = reinterpret_cast<MppPacket>(&packet_data);
	rc = mpi->encode_get_packet(ctx, &packet);
	if (rc != MPP_OK || !packet_data.len) {
		char msg[96];
		std::snprintf(msg, sizeof(msg), "MPP 获取 H265 码流失败: rc=%d", rc);
		error_ = msg;
		if (rc == MPP_OK)
			mpi->encode_release_packet(ctx, &packet);
		return -1;
	}

	struct valloc_mb mb{};
	mb.mpi_buf_id = static_cast<int>(packet_data.u64priv_data);
	mb.struct_size = sizeof(mb);
	if (::ioctl(valloc_fd_, VALLOC_IOCTL_MB_GET_FD, &mb) < 0 ||
	    mb.dma_buf_fd < 0) {
		error_ = "MPP 输出 DMA-BUF 映射失败";
		mpi->encode_release_packet(ctx, &packet);
		return -1;
	}
	void *mapped = ::mmap(nullptr, packet_data.buf_size, PROT_READ,
			      MAP_SHARED, mb.dma_buf_fd, 0);
	if (mapped == MAP_FAILED) {
		::close(mb.dma_buf_fd);
		error_ = "MPP 输出码流 mmap 失败";
		mpi->encode_release_packet(ctx, &packet);
		return -1;
	}
	size_t offset = packet_data.offset;
	if (offset + packet_data.len > packet_data.buf_size)
		offset = 0;
	const size_t wrote = std::fwrite(static_cast<uint8_t *>(mapped) + offset,
					1, packet_data.len, raw_file_);
	std::fflush(raw_file_);
	::munmap(mapped, packet_data.buf_size);
	::close(mb.dma_buf_fd);

	const MPP_RET rel_rc = mpi->encode_release_packet(ctx, &packet);
	if (rel_rc != MPP_OK) {
		error_ = "MPP 释放输出码流包失败（输出缓冲可能未回池）";
		return -1;
	}
	if (wrote != packet_data.len) {
		error_ = "写 H265 临时码流失败";
		return -1;
	}
	frames_.fetch_add(1);
	/*
	 * 常驻心跳：每 30 帧一条，同时兼作 fd 存活哨兵。
	 * alive=0 说明 V4L2 的 DMA-BUF fd 又被谁关掉了 —— 曾经踩过：
	 * mpp_buffer_import_with_tag() 接管 info.fd 所有权，buffer_put() 时把
	 * 它 close，结果 4 帧后 ISP 停止出帧、采集线程永久卡在 poll()。
	 */
	if ((fno + 1) % 30 == 0) {
		const int alive = ::fcntl(fd, F_GETFD) >= 0 ? 1 : 0;
		std::fprintf(stderr,
			     "[APP ] H265 零拷贝：%d 帧完成，src_fd=%d alive=%d\n",
			     fno + 1, fd, alive);
		std::fflush(stderr);
	}
	return 0;
}

void H265Recorder::cleanup_encoder()
{
	if (mpp_cfg_) {
		api_->cfg_deinit(static_cast<MppEncCfg>(mpp_cfg_));
		mpp_cfg_ = nullptr;
	}
	if (mpp_ctx_) {
		auto *mpi = static_cast<MppApi *>(mpp_api_);
		if (mpi)
			mpi->reset(static_cast<MppCtx>(mpp_ctx_));
		api_->destroy(static_cast<MppCtx>(mpp_ctx_));
		mpp_ctx_ = nullptr;
		mpp_api_ = nullptr;
	}
	if (valloc_fd_ >= 0) {
		::close(valloc_fd_);
		valloc_fd_ = -1;
	}
	if (raw_file_) {
		std::fclose(raw_file_);
		raw_file_ = nullptr;
	}
}

int H265Recorder::remux_mp4()
{
	char fps[16];
	char output[512];
	std::snprintf(fps, sizeof(fps), "%d", fps_);
	const char *const argv[] = {
		"/usr/bin/ffmpeg", "-loglevel", "error", "-framerate", fps,
		"-i", raw_path_.c_str(), "-c:v", "copy", "-tag:v", "hvc1",
		"-movflags", "+faststart", "-y", mp4_path_.c_str(), nullptr
	};
	const int rc = res::run_argv(argv, output, sizeof(output), true);
	if (rc == 0) {
		::unlink(raw_path_.c_str());
		return 0;
	}
	error_ = std::string("MP4 封装失败: ") + output;
	return -1;
}

int H265Recorder::stop()
{
	if (!active_.exchange(false))
		return -1;
	cleanup_encoder();
	return frames_.load() > 0 ? remux_mp4() : -1;
}

} /* namespace camp */
