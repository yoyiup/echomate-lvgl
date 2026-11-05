/**
 * motor_ctl.cpp —— 电机控制（C++ 类 + 一张 C 函数表）
 *
 * 设计（和"套外壳"的写法有什么区别）：
 *   · 状态和操作都放在 MotorDevice 里，fd 由 res::Fd 管，
 *     析构自动 close —— 资源生命周期是 C++ 在负责，不是靠人记着关。
 *   · 对外只暴露 motor_api() 一个 C 符号，返回静态只读函数表。
 *     界面层拿到表之后 `m->set(...)`，和调结构体方法一样。
 *   · 仍然不抛异常：uClibc + C 混编，异常穿越 C 边界是 UB。
 *     所有失败走"返回 -errno + last_error() 记一句中文"。
 *
 * 【驱动的行为要清楚】
 *   ECHO_MOTOR_SET 是"设方向"，不是"按下去才转"。写一次就一直保持，
 *   直到下一次写入或模块卸载（exit 里会 stop_all）。
 *   所以页面上必须有显眼的"全部停止"，并且离开页面时强制停 ——
 *   否则用户退出界面后电机还在转。
 */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "motor_ctl.h"
#include "../common/resources.hpp"

#define MOTOR_DEV "/dev/echo_motor"

namespace {

class MotorDevice {
public:
	MotorDevice() = default;

	/* 单例，别让它被拷贝 */
	MotorDevice(const MotorDevice &) = delete;
	MotorDevice &operator=(const MotorDevice &) = delete;

	int open()
	{
		if (fd_.valid())
			return 0;

		int fd = ::open(MOTOR_DEV, O_RDWR | O_CLOEXEC);

		if (fd < 0) {
			int e = errno;

			putErr("打不开 %s：%s（驱动装了吗？insmod motor.ko）",
			       MOTOR_DEV, std::strerror(e));
			return -e;
		}

		fd_.reset(fd);
		err_[0] = '\0';
		return 0;
	}

	bool isOpen() const { return fd_.valid(); }

	int set(int id, int dir)
	{
		if (id < 0 || id >= ECHO_MOTOR_MAX ||
		    dir < ECHO_MOTOR_COAST || dir > ECHO_MOTOR_BRAKE) {
			putErr("参数越界：id=%d dir=%d", id, dir);
			return -EINVAL;
		}

		int rc = open();

		if (rc)
			return rc;

		struct echo_motor_req req;

		std::memset(&req, 0, sizeof(req));
		req.id  = static_cast<__u8>(id);
		req.dir = static_cast<__u8>(dir);
		req.pad = 0;

		if (ioctl(fd_.get(), ECHO_MOTOR_SET, &req) < 0)
			return ioctlFail("设置方向");

		return 0;
	}

	int stopAll()
	{
		/* 没开过就开一次，避免"退出时想停却没 fd" */
		int rc = open();

		if (rc)
			return rc;

		if (ioctl(fd_.get(), ECHO_MOTOR_STOP_ALL, 0) < 0)
			return ioctlFail("全部停止");

		return 0;
	}

	int query(int dirs[ECHO_MOTOR_MAX])
	{
		if (!dirs)
			return -EINVAL;

		int rc = open();

		if (rc)
			return rc;

		struct echo_motor_status st;

		std::memset(&st, 0, sizeof(st));

		if (ioctl(fd_.get(), ECHO_MOTOR_GET, &st) < 0)
			return ioctlFail("读状态");

		for (int i = 0; i < ECHO_MOTOR_MAX; i++)
			dirs[i] = st.dir[i];

		return 0;
	}

	const char *lastError() const { return err_; }

private:
	void putErr(const char *fmt, ...)
	{
		va_list ap;

		va_start(ap, fmt);
		std::vsnprintf(err_, sizeof(err_), fmt, ap);
		va_end(ap);
	}

	/*
	 * ioctl 失败后的统一处理。
	 *
	 * 【为什么先存 errno】
	 *   res::Fd::reset() 内部会 close()，close() 可能改写 errno，
	 *   之后再报错就是错的错误码了。
	 *
	 * 【为什么 ENODEV/EBADF/EIO 要主动关 fd】
	 *   驱动被 rmmod 后 fd 还在，但已经不是那个设备了。留着它下次 ioctl
	 *   仍然失败，界面会一直显示旧错误。直接关掉，下次调用会重新 open，
	 *   模块重新 insmod 之后点一下按钮就恢复了 —— 不用重启应用。
	 */
	int ioctlFail(const char *what)
	{
		int e = errno;

		putErr("%s 失败：%s", what, std::strerror(e));

		if (e == ENODEV || e == EBADF || e == EIO)
			fd_.reset();

		return -e;
	}

	res::Fd	fd_;
	char	err_[128] = { 0 };
};

MotorDevice g_dev;

/* ---- 函数表：把成员函数包成 C 能调的形式 ---- */
int api_open(void) { return g_dev.open(); }
int api_set(int id, int dir) { return g_dev.set(id, dir); }
int api_stop_all(void) { return g_dev.stopAll(); }
int api_query(int dirs[ECHO_MOTOR_MAX]) { return g_dev.query(dirs); }
int api_is_open(void) { return g_dev.isOpen() ? 1 : 0; }
const char *api_last_error(void) { return g_dev.lastError(); }

const struct motor_ops kOps = {
	api_open,
	api_set,
	api_stop_all,
	api_query,
	api_is_open,
	api_last_error,
};

} /* namespace */

extern "C" const struct motor_ops *motor_api(void)
{
	return &kOps;
}
