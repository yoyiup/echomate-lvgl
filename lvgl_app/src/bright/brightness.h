/**
 * brightness.h —— 屏幕背光调节（C 侧看到的只有一张函数表）
 *
 * 和 motor_ctl.h 同一套结构：实现是 C++ 类，对外只留 bright_api() 一个符号。
 * 界面用法：
 *       const struct bright_ops *b = bright_api();
 *       b->set(40);
 *
 * 板子上背光是标准 sysfs 设备：
 *     /sys/class/backlight/backlight/{brightness,max_brightness}
 *     实测：brightness=50，max_brightness=99
 */
#ifndef __BRIGHTNESS_H__
#define __BRIGHTNESS_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ★ 最低亮度锁在 10，不允许调到 0。
 *
 *   背光调到 0 就是全黑 —— 而这块屏是唯一的输出来源，
 *   屏幕一黑用户看不见滑块在哪，也看不见"返回"，等于把自己锁死在外面。
 *   下限在【这一层】统一钳，界面、按钮、以后别的调用方都跑不掉。
 */
#define BRIGHT_MIN	10

struct bright_ops {
	/* 背光最大档位。读不到设备时返回 99（板子实测值） */
	int (*max)(void);

	/* 当前亮度；读失败返回 -1 */
	int (*get)(void);

	/* 设置亮度（自动钳到 BRIGHT_MIN ~ max）。返回实际写入值，<0 失败 */
	int (*set)(int v);

	/* 设备是否可写（1/0）。不可用时界面应显示提示而不是假装能调 */
	int (*available)(void);
};

/* 全模块唯一的 C 入口 */
const struct bright_ops *bright_api(void);

#ifdef __cplusplus
}
#endif

#endif /* __BRIGHTNESS_H__ */
