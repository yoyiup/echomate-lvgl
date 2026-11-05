#ifndef H265_RECORDER_HPP
#define H265_RECORDER_HPP

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace camp {

/* RV1106 Rockit/VENC 硬件 H.265 编码器。实现放在 .cpp，避免 Rockchip
 * 的大批 C 头文件污染摄像头主类。 */
class H265Recorder {
public:
	H265Recorder() = default;
	~H265Recorder();

	H265Recorder(const H265Recorder &) = delete;
	H265Recorder &operator=(const H265Recorder &) = delete;

	int start(const std::string &mp4_path, int width, int height, int fps);
	int submit(const uint8_t *y, const uint8_t *uv, uint64_t pts_us);
	int stop();
	bool active() const { return active_.load(); }
	int frames() const { return frames_.load(); }
	const std::string &last_error() const { return error_; }

private:
	void output_loop();
	void cleanup_encoder();
	int load_api();
	int remux_mp4();

	struct Api;
	Api *api_ = nullptr;
	void *mpp_lib_ = nullptr;
	void *rga_lib_ = nullptr;
	void *rockit_lib_ = nullptr;
	FILE *raw_file_ = nullptr;
	std::thread output_thread_;
	std::atomic<bool> active_{ false };
	std::atomic<int> frames_{ 0 };
	bool sys_inited_ = false;
	bool channel_created_ = false;
	int width_ = 0;
	int height_ = 0;
	int fps_ = 0;
	std::string mp4_path_;
	std::string raw_path_;
	std::string error_;
};

} /* namespace camp */

#endif
