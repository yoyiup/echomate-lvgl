#include "h265_recorder.hpp"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

#include "resources.hpp"

extern "C" {
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
}

namespace camp {

static constexpr int kVencChannel = 0;
static constexpr const char *kCodecDir = "/tmp/cam_codec";

struct H265Recorder::Api {
	RK_S32 (*sys_init)(void);
	RK_S32 (*sys_exit)(void);
	RK_S32 (*mmz_alloc)(MB_BLK *, const RK_CHAR *, const RK_CHAR *, RK_U32);
	RK_S32 (*mmz_free)(MB_BLK);
	RK_S32 (*flush)(MB_BLK, RK_BOOL);
	RK_VOID *(*mb_addr)(MB_BLK);
	RK_S32 (*create)(VENC_CHN, const VENC_CHN_ATTR_S *);
	RK_S32 (*destroy)(VENC_CHN);
	RK_S32 (*start_recv)(VENC_CHN, const VENC_RECV_PIC_PARAM_S *);
	RK_S32 (*stop_recv)(VENC_CHN);
	RK_S32 (*send)(VENC_CHN, const VIDEO_FRAME_INFO_S *, RK_S32);
	RK_S32 (*get)(VENC_CHN, VENC_STREAM_S *, RK_S32);
	RK_S32 (*release)(VENC_CHN, VENC_STREAM_S *);
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
	delete api_;
	if (rockit_lib_)
		::dlclose(rockit_lib_);
	if (rga_lib_)
		::dlclose(rga_lib_);
	if (mpp_lib_)
		::dlclose(mpp_lib_);
}

int H265Recorder::load_api()
{
	if (api_)
		return 0;

	const std::string mpp = std::string(kCodecDir) + "/librockchip_mpp.so.1";
	const std::string rga = std::string(kCodecDir) + "/librga.so";
	const std::string rockit = std::string(kCodecDir) + "/librockit.so";

	mpp_lib_ = ::dlopen(mpp.c_str(), RTLD_NOW | RTLD_GLOBAL);
	rga_lib_ = ::dlopen(rga.c_str(), RTLD_NOW | RTLD_GLOBAL);
	rockit_lib_ = ::dlopen(rockit.c_str(), RTLD_NOW | RTLD_GLOBAL);
	if (!mpp_lib_ || !rga_lib_ || !rockit_lib_) {
		const char *why = ::dlerror();
		error_ = std::string("H265 运行库未就绪: ") + (why ? why : "dlopen");
		return -1;
	}

	api_ = new Api{};
#define LOAD(name, field) if (!sym(rockit_lib_, name, api_->field)) goto missing
	LOAD("RK_MPI_SYS_Init", sys_init);
	LOAD("RK_MPI_SYS_Exit", sys_exit);
	LOAD("RK_MPI_SYS_MmzAlloc_Cached", mmz_alloc);
	LOAD("RK_MPI_SYS_MmzFree", mmz_free);
	LOAD("RK_MPI_SYS_MmzFlushCache", flush);
	LOAD("RK_MPI_MB_Handle2VirAddr", mb_addr);
	LOAD("RK_MPI_VENC_CreateChn", create);
	LOAD("RK_MPI_VENC_DestroyChn", destroy);
	LOAD("RK_MPI_VENC_StartRecvFrame", start_recv);
	LOAD("RK_MPI_VENC_StopRecvFrame", stop_recv);
	LOAD("RK_MPI_VENC_SendFrame", send);
	LOAD("RK_MPI_VENC_GetStream", get);
	LOAD("RK_MPI_VENC_ReleaseStream", release);
#undef LOAD
	return 0;

missing:
	error_ = "H265 运行库接口不完整";
	delete api_;
	api_ = nullptr;
	return -1;
}

int H265Recorder::start(const std::string &mp4_path, int width, int height, int fps)
{
	if (active_.load() || mp4_path.empty() || width <= 0 || height <= 0)
		return -1;
	/* 编码组件压缩保存在闪存，首次录像时展开/装载。这样普通相机预览
	 * 不额外占用 tmpfs，也不要求守护脚本设置动态库环境。 */
	if (::access("/dev/mpi/valloc", R_OK | W_OK) != 0 ||
	    ::access((std::string(kCodecDir) + "/librockit.so").c_str(), R_OK) != 0 ||
	    ::access("/usr/lib/librockchip_mpp.so.1", R_OK) != 0 ||
	    ::access("/usr/lib/librga.so", R_OK) != 0) {
		char output[256];
		const char *const argv[] = { "/root/bin/load_h265.sh", nullptr };
		if (res::run_argv(argv, output, sizeof(output), true) != 0) {
			error_ = std::string("H265 编码环境加载失败: ") + output;
			return -1;
		}
	}
	if (load_api() != 0)
		return -1;
	if (::access("/dev/mpp_service", R_OK | W_OK) != 0 ||
	    ::access("/dev/mpi/valloc", R_OK | W_OK) != 0) {
		error_ = "H265 编码驱动未加载";
		return -1;
	}

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

	if (api_->sys_init() != RK_SUCCESS) {
		error_ = "RK_MPI_SYS_Init 失败";
		cleanup_encoder();
		return -1;
	}
	sys_inited_ = true;

	VENC_CHN_ATTR_S attr{};
	attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
	attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	attr.stVencAttr.u32MaxPicWidth = width_;
	attr.stVencAttr.u32MaxPicHeight = height_;
	attr.stVencAttr.u32PicWidth = width_;
	attr.stVencAttr.u32PicHeight = height_;
	attr.stVencAttr.u32VirWidth = (width_ + 1) & ~1;
	attr.stVencAttr.u32VirHeight = (height_ + 1) & ~1;
	attr.stVencAttr.u32StreamBufCnt = 3;
	attr.stVencAttr.u32BufSize = width_ * height_ * 3 / 2;
	attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
	attr.stRcAttr.stH265Cbr.u32Gop = fps_;
	attr.stRcAttr.stH265Cbr.u32BitRate = 512; /* 320x240：512 kbps */
	attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = fps_;
	attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
	attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = fps_;
	attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;

	if (api_->create(kVencChannel, &attr) != RK_SUCCESS) {
		error_ = "创建 H265 硬件编码通道失败";
		cleanup_encoder();
		return -1;
	}
	channel_created_ = true;

	VENC_RECV_PIC_PARAM_S recv{};
	recv.s32RecvPicNum = -1;
	if (api_->start_recv(kVencChannel, &recv) != RK_SUCCESS) {
		error_ = "启动 H265 编码通道失败";
		cleanup_encoder();
		return -1;
	}

	active_.store(true);
	output_thread_ = std::thread(&H265Recorder::output_loop, this);
	return 0;
}

int H265Recorder::submit(const uint8_t *y, const uint8_t *uv, uint64_t pts_us)
{
	if (!active_.load() || !y || !uv)
		return -1;

	const size_t y_size = static_cast<size_t>(width_) * height_;
	const size_t frame_size = y_size * 3 / 2;
	MB_BLK block = RK_NULL;
	if (api_->mmz_alloc(&block, "cam_h265", nullptr,
			    static_cast<RK_U32>(frame_size)) != RK_SUCCESS || !block)
		return -1;

	auto *dst = static_cast<uint8_t *>(api_->mb_addr(block));
	if (!dst) {
		api_->mmz_free(block);
		return -1;
	}
	std::memcpy(dst, y, y_size);
	std::memcpy(dst + y_size, uv, frame_size - y_size);
	api_->flush(block, RK_FALSE);

	VIDEO_FRAME_INFO_S frame{};
	frame.stVFrame.pMbBlk = block;
	frame.stVFrame.u32Width = width_;
	frame.stVFrame.u32Height = height_;
	frame.stVFrame.u32VirWidth = width_;
	frame.stVFrame.u32VirHeight = height_;
	frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
	frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	frame.stVFrame.u64PTS = pts_us;

	const RK_S32 rc = api_->send(kVencChannel, &frame, 100);
	api_->mmz_free(block);
	return rc == RK_SUCCESS ? 0 : -1;
}

void H265Recorder::output_loop()
{
	VENC_PACK_S pack{};
	VENC_STREAM_S stream{};
	stream.pstPack = &pack;

	while (active_.load()) {
		if (api_->get(kVencChannel, &stream, 100) != RK_SUCCESS)
			continue;
		void *data = api_->mb_addr(pack.pMbBlk);
		if (data && pack.u32Len && raw_file_) {
			std::fwrite(data, 1, pack.u32Len, raw_file_);
			frames_.fetch_add(1);
		}
		api_->release(kVencChannel, &stream);
	}
}

void H265Recorder::cleanup_encoder()
{
	if (channel_created_) {
		api_->stop_recv(kVencChannel);
		api_->destroy(kVencChannel);
		channel_created_ = false;
	}
	if (sys_inited_) {
		api_->sys_exit();
		sys_inited_ = false;
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
	if (output_thread_.joinable())
		output_thread_.join();
	cleanup_encoder();
	return frames_.load() > 0 ? remux_mp4() : -1;
}

} /* namespace camp */
