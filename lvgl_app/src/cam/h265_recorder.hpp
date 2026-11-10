#ifndef H265_RECORDER_HPP
#define H265_RECORDER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace camp {

/* V4L2 DMA-BUF -> Rockchip MPP 硬件 H.265。输入帧不经过 CPU 复制。 */
class H265Recorder {
public:
	H265Recorder() = default;
	~H265Recorder();

	H265Recorder(const H265Recorder &) = delete;
	H265Recorder &operator=(const H265Recorder &) = delete;

	int start(const std::string &mp4_path, int width, int height, int fps);
	int submit_dmabuf(int fd, uint8_t *addr, size_t bytes, uint64_t pts_us);
	/* MPP 使用阻塞输入模式；submit 返回时硬件已读完输入 DMA-BUF。 */
	bool buffer_idle(int) const { return true; }
	int stop();
	bool active() const { return active_.load(); }
	int frames() const { return frames_.load(); }
	const std::string &last_error() const { return error_; }

private:
	struct Api;
	int load_api();
	void cleanup_encoder();
	int remux_mp4();

	Api *api_ = nullptr;
	void *mpp_lib_ = nullptr;
	void *mpp_ctx_ = nullptr;
	void *mpp_api_ = nullptr;
	void *mpp_cfg_ = nullptr;
	FILE *raw_file_ = nullptr;
	int valloc_fd_ = -1;
	std::atomic<bool> active_{ false };
	std::atomic<int> frames_{ 0 };
	int width_ = 0;
	int height_ = 0;
	int fps_ = 0;
	std::string mp4_path_;
	std::string raw_path_;
	std::string error_;
};

} /* namespace camp */

#endif
