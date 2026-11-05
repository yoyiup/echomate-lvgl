/**
 * brightness.cpp —— 背光 sysfs 读写（C++ 类 + 一张 C 函数表）
 *
 * 【为什么路径写死】
 *   标准做法是遍历 /sys/class/backlight/* 取第一个，但这块板上只有一个
 *   设备、名字就叫 backlight（/sys/devices/platform/backlight/...）。
 *   写死省一次遍历，也让"到底操作哪个设备"在代码里是明面的。
 *   真要泛化再改成 readdir，接口不用变。
 *
 * 【为什么每次 set 都重新 open】
 *   sysfs 属性文件不支持 lseek/复用写，必须一次 open 一次 write 一次 close；
 *   而背光只在人手动滑动时才写，open 的开销可以忽略。
 */
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "brightness.h"

#define BL_BRIGHT	"/sys/class/backlight/backlight/brightness"
#define BL_MAX		"/sys/class/backlight/backlight/max_brightness"

namespace {

class Backlight {
public:
	int max() const { return readInt(BL_MAX, 99); }

	bool available() const { return ::access(BL_BRIGHT, W_OK) == 0; }

	int get() const
	{
		if (::access(BL_BRIGHT, R_OK) != 0)
			return -1;
		return readInt(BL_BRIGHT, -1);
	}

	int set(int v)
	{
		int mx = max();

		/* 下限 10 见 brightness.h：0 会把自己关在全黑里出不来 */
		if (v < BRIGHT_MIN)
			v = BRIGHT_MIN;
		if (v > mx)
			v = mx;

		int fd = ::open(BL_BRIGHT, O_WRONLY);

		if (fd < 0)
			return -errno;

		char buf[16];
		int len = std::snprintf(buf, sizeof(buf), "%d\n", v);
		int n = static_cast<int>(::write(fd, buf, len));

		::close(fd);

		return n == len ? v : -EIO;
	}

private:
	static int readInt(const char *path, int dflt)
	{
		FILE *f = std::fopen(path, "r");
		int v;

		if (!f)
			return dflt;
		if (std::fscanf(f, "%d", &v) != 1)
			v = dflt;
		std::fclose(f);
		return v;
	}
};

Backlight g_bl;

int api_max(void) { return g_bl.max(); }
int api_get(void) { return g_bl.get(); }
int api_set(int v) { return g_bl.set(v); }
int api_available(void) { return g_bl.available() ? 1 : 0; }

const struct bright_ops kOps = {
	api_max,
	api_get,
	api_set,
	api_available,
};

} /* namespace */

extern "C" const struct bright_ops *bright_api(void)
{
	return &kOps;
}
