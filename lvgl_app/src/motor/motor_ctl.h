/**
 * motor_ctl.h —— 电机控制（C 侧看到的只有一张函数表）
 *
 * 【为什么是这个形状】
 *   实现是 C++ 类（MotorDevice，内部用 RAII 管 fd），而界面层是 C，
 *   C 没法直接调成员函数。传统做法是给每个成员函数写一个
 *   `extern "C"` 包装 —— 那就是"套外壳"：158 行里 7 个 extern "C"，
 *   本质还是 C 过程式代码。
 *
 *   现在改成：整块能力用一张【只读函数表】暴露，
 *   **整个模块只有 motor_api() 一个 C 符号**。
 *   界面这样用：
 *       const struct motor_ops *m = motor_api();
 *       m->set(ECHO_MOTOR_LEFT, ECHO_MOTOR_FWD);
 *
 *   这也是 Linux 内核里常见的手法（file_operations 本身就是函数表），
 *   新增功能只是往表里加一行，不用再添一个全局符号。
 *
 * 【枚举/结构体/ioctl 号从哪来】
 *   直接用内核的 motor_uapi.h（相对路径引），【不在这里再抄一份】。
 *   抄一份必然出现"驱动改了应用没改"，而且 _IOW 的 size 一错就是
 *   ioctl 返回 -ENOTTY，症状是"按钮没反应"，极难查。
 */
#ifndef ECHO_MOTOR_CTL_H
#define ECHO_MOTOR_CTL_H

#include "../../../kernel/my/motor_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct motor_ops {
	/* 打开 /dev/echo_motor（惰性，重复调用返回 0）。返回 0 或 -errno */
	int  (*open)(void);

	/* 设置方向：id = ECHO_MOTOR_LEFT/RIGHT，dir = COAST/FWD/REV/BRAKE */
	int  (*set)(int id, int dir);

	/* 两路都滑行停止 */
	int  (*stop_all)(void);

	/* 读回两路当前方向（dirs[0]=左 dirs[1]=右） */
	int  (*query)(int dirs[ECHO_MOTOR_MAX]);

	/* 是否已拿到有效 fd（0/1） */
	int  (*is_open)(void);

	/* 最近一次失败的中文描述（没失败时是空串） */
	const char *(*last_error)(void);
};

/* 全模块唯一的 C 入口。返回常驻的只读函数表，永远不会是 NULL。 */
const struct motor_ops *motor_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ECHO_MOTOR_CTL_H */
