#ifndef JPEG_ENCODER_HPP
#define JPEG_ENCODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace camp {

/* 单帧 NV12 DMA-BUF -> Rockchip MPP 硬件 JPEG。 */
class JpegEncoder {
public:
	JpegEncoder() = default;
	~JpegEncoder();

	JpegEncoder(const JpegEncoder &) = delete;
	JpegEncoder &operator=(const JpegEncoder &) = delete;

	int encode_dmabuf(const std::string &path, int fd, uint8_t *addr,
			  size_t bytes, int width, int height, int quality = 90);
	const std::string &last_error() const { return error_; }

private:
	struct Api;
	int load_api();

	Api *api_ = nullptr;
	void *mpp_lib_ = nullptr;
	std::string error_;
};

} /* namespace camp */

#endif
