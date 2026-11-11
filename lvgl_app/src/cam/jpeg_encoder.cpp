#include "jpeg_encoder.hpp"

#include <cerrno>
#include <cstdio>
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
#include <rockchip/rk_venc_rc.h>
}

namespace camp {

static constexpr const char *kCodecDir = "/tmp/cam_codec";
static constexpr const char *kTag = "echomate_jpeg";

struct JpegEncoder::Api {
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
	void (*frame_buffer)(MppFrame, MppBuffer);
	MPP_RET (*cfg_init)(MppEncCfg *);
	MPP_RET (*cfg_deinit)(MppEncCfg);
	MPP_RET (*cfg_s32)(MppEncCfg, const char *, RK_S32);
};

template <typename T>
static bool load_symbol(void *lib, const char *name, T &out)
{
	out = reinterpret_cast<T>(::dlsym(lib, name));
	return out != nullptr;
}

JpegEncoder::~JpegEncoder()
{
	delete api_;
	if (mpp_lib_)
		::dlclose(mpp_lib_);
}

int JpegEncoder::load_api()
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
#define LOAD(name, field) if (!load_symbol(mpp_lib_, name, api_->field)) goto missing
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
	LOAD("mpp_frame_set_buffer", frame_buffer);
	LOAD("mpp_enc_cfg_init", cfg_init);
	LOAD("mpp_enc_cfg_deinit", cfg_deinit);
	LOAD("mpp_enc_cfg_set_s32", cfg_s32);
#undef LOAD
	return 0;

missing:
	error_ = "MPP JPEG 运行库接口不完整";
	delete api_;
	api_ = nullptr;
	return -1;
}

int JpegEncoder::encode_dmabuf(const std::string &path, int fd, uint8_t *addr,
				 size_t bytes, int width, int height, int quality)
{
	const size_t need = static_cast<size_t>(width) * height * 3 / 2;
	if (path.empty() || fd < 0 || !addr || width != 640 || height != 480 ||
	    bytes < need) {
		error_ = "JPEG 输入必须是 640x480 NV12 DMA-BUF";
		return -1;
	}
	if (quality < 1)
		quality = 1;
	if (quality > 99)
		quality = 99;

	if (::access("/dev/mpi/valloc", R_OK | W_OK) != 0 ||
	    ::access("/dev/mpi/venc", R_OK | W_OK) != 0 ||
	    ::access((std::string(kCodecDir) + "/librockchip_mpp.so.1").c_str(), R_OK) != 0) {
		char output[256];
		const char *const argv[] = { "/root/bin/load_h265.sh", nullptr };
		if (res::run_argv(argv, output, sizeof(output), true) != 0) {
			error_ = std::string("JPEG 编码环境加载失败: ") + output;
			return -1;
		}
	}
	if (load_api() != 0)
		return -1;

	MppCtx ctx = nullptr;
	MppApi *mpi = nullptr;
	MppEncCfg cfg = nullptr;
	MppBuffer input = nullptr;
	MppFrame frame = nullptr;
	int import_fd = -1;
	int valloc_fd = -1;
	FILE *out = nullptr;
	int rc_out = -1;
	RK_S64 output_timeout_ms = 3000;

	if (api_->create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
		error_ = "mpp_create JPEG 失败";
		goto done;
	}
	if (api_->init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
		error_ = "mpp_init JPEG 失败";
		goto done;
	}
	if (api_->cfg_init(&cfg) != MPP_OK ||
	    mpi->control(ctx, MPP_ENC_GET_CFG, cfg) != MPP_OK) {
		error_ = "读取 MPP JPEG 配置失败";
		goto done;
	}
	api_->cfg_s32(cfg, "prep:width", width);
	api_->cfg_s32(cfg, "prep:height", height);
	api_->cfg_s32(cfg, "prep:hor_stride", width);
	api_->cfg_s32(cfg, "prep:ver_stride", height);
	api_->cfg_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	/* 这版 RV1106 内核编码驱动即使做单帧 JPEG，也会进入 RC 模块。
	 * fps 分母保持默认 0 会触发内核断言，随后编码线程空指针崩溃；所以
	 * 必须像官方 mpi_enc_test 一样给出完整且有效的 RC 时基。JPEG 本身
	 * 用固定质量，选择 FIXQP，1/1 fps 只用于满足驱动的合法性检查。 */
	api_->cfg_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
	api_->cfg_s32(cfg, "rc:fps_in_flex", 0);
	api_->cfg_s32(cfg, "rc:fps_in_num", 1);
	api_->cfg_s32(cfg, "rc:fps_in_denorm", 1);
	api_->cfg_s32(cfg, "rc:fps_out_flex", 0);
	api_->cfg_s32(cfg, "rc:fps_out_num", 1);
	api_->cfg_s32(cfg, "rc:fps_out_denorm", 1);
	api_->cfg_s32(cfg, "rc:gop", 1);
	api_->cfg_s32(cfg, "codec:type", MPP_VIDEO_CodingMJPEG);
	api_->cfg_s32(cfg, "jpeg:q_factor", quality);
	api_->cfg_s32(cfg, "jpeg:qf_min", quality);
	api_->cfg_s32(cfg, "jpeg:qf_max", quality);
	if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
		error_ = "设置 MPP JPEG 参数失败";
		goto done;
	}
	if (mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout_ms) != MPP_OK) {
		error_ = "设置 JPEG 输出超时失败";
		goto done;
	}

	/* MPP 会在 buffer_put 时关闭导入 fd，只能把 dup 副本交给它。dup 仍
	 * 指向同一块物理 DMA-BUF，所以没有任何像素拷贝。 */
	import_fd = ::dup(fd);
	if (import_fd < 0) {
		error_ = "dup 摄像头 DMA-BUF fd 失败";
		goto done;
	}
	{
		MppBufferInfo info{};
		info.type = MPP_BUFFER_TYPE_ION;
		info.size = bytes;
		info.ptr = addr;
		info.fd = import_fd;
		info.index = -1;
		if (api_->buffer_import(nullptr, &info, &input, kTag, "photo") != MPP_OK ||
		    !input) {
			error_ = "MPP 导入拍照 DMA-BUF 失败";
			goto done;
		}
		import_fd = -1; /* 所有权已交给 MPP */
	}
	if (api_->frame_init(&frame) != MPP_OK || !frame) {
		error_ = "创建 JPEG 输入帧失败";
		goto done;
	}
	api_->frame_width(frame, width);
	api_->frame_height(frame, height);
	api_->frame_hstride(frame, width);
	api_->frame_vstride(frame, height);
	api_->frame_fmt(frame, MPP_FMT_YUV420SP);
	api_->frame_buffer(frame, input);
	if (mpi->encode_put_frame(ctx, frame) != MPP_OK) {
		error_ = "提交 JPEG 输入帧失败";
		goto done;
	}
	api_->frame_deinit(&frame);

	{
		struct venc_packet packet_data{};
		MppPacket packet = reinterpret_cast<MppPacket>(&packet_data);
		if (mpi->encode_get_packet(ctx, &packet) != MPP_OK || !packet_data.len) {
			error_ = "获取 JPEG 码流失败";
			goto done;
		}

		valloc_fd = ::open("/dev/mpi/valloc", O_RDWR | O_CLOEXEC);
		struct valloc_mb mb{};
		mb.mpi_buf_id = static_cast<int>(packet_data.u64priv_data);
		mb.struct_size = sizeof(mb);
		if (valloc_fd < 0 || ::ioctl(valloc_fd, VALLOC_IOCTL_MB_GET_FD, &mb) < 0 ||
		    mb.dma_buf_fd < 0) {
			error_ = "MPP JPEG 输出 DMA-BUF 映射失败";
			mpi->encode_release_packet(ctx, &packet);
			goto done;
		}
		void *mapped = ::mmap(nullptr, packet_data.buf_size, PROT_READ,
				      MAP_SHARED, mb.dma_buf_fd, 0);
		if (mapped == MAP_FAILED) {
			::close(mb.dma_buf_fd);
			error_ = "MPP JPEG 输出 mmap 失败";
			mpi->encode_release_packet(ctx, &packet);
			goto done;
		}

		out = std::fopen(path.c_str(), "wb");
		size_t wrote = 0;
		if (out && packet_data.buf_size && packet_data.offset < packet_data.buf_size &&
		    packet_data.len <= packet_data.buf_size) {
			const size_t tail = packet_data.buf_size - packet_data.offset;
			const size_t first = packet_data.len < tail ? packet_data.len : tail;
			wrote = std::fwrite(static_cast<uint8_t *>(mapped) + packet_data.offset,
					    1, first, out);
			if (wrote == first && packet_data.len > first)
				wrote += std::fwrite(mapped, 1, packet_data.len - first, out);
		}
		::munmap(mapped, packet_data.buf_size);
		::close(mb.dma_buf_fd);
		const MPP_RET release_rc = mpi->encode_release_packet(ctx, &packet);
		if (out) {
			const int close_rc = std::fclose(out);
			out = nullptr;
			if (close_rc != 0)
				wrote = 0;
		}
		if (release_rc != MPP_OK || wrote != packet_data.len) {
			error_ = "写 JPEG 文件失败";
			::unlink(path.c_str());
			goto done;
		}
	}

	error_.clear();
	rc_out = 0;

done:
	if (out)
		std::fclose(out);
	if (frame)
		api_->frame_deinit(&frame);
	/* 必须等输出包取回后再释放 input，硬件此时才一定读完该 DMA-BUF。 */
	if (input)
		api_->buffer_put(input, "photo");
	else if (import_fd >= 0)
		::close(import_fd);
	if (cfg)
		api_->cfg_deinit(cfg);
	if (ctx)
		api_->destroy(ctx);
	if (valloc_fd >= 0)
		::close(valloc_fd);
	return rc_out;
}

} /* namespace camp */
