/**
 * ui.h —— 多页面 UI 的公共接口
 *
 * 【为什么要拆成模块】
 * 原来所有界面代码都堆在 main.c 里（700 行），加第二个页面就会失控。
 * 现在按"职责"拆开：
 *   ui_theme.c  颜色 / 公共控件 / 页面导航（这层是"框架"）
 *   ui_home.c   主页设置列表
 *   ui_pages.c  网络 / 音量 / 关于 三个子页
 *   main.c      只做 lv_init + 接 fbdev/evdev + 调 ui_page_home()
 *
 * 【LVGL 9 的多页面是什么】
 * 一个 "screen" 就是一个 lv_obj_create(NULL) 出来的根对象，彼此独立。
 * lv_screen_load_anim() 把某个 screen 设为当前显示的那个，
 * 其它 screen 还在内存里，只是不参与渲染也不占 CPU。
 * 所以切回上一页时控件状态（开关位置、滑块值）原样保留 —— 不用手动存。
 */
#ifndef __UI_H__
#define __UI_H__

#include <stdbool.h>
#include "lvgl/lvgl.h"

/* ---------- 屏幕尺寸 ---------- */
#define UI_W 320
#define UI_H 240

/* ---------- 主题色 ----------
 * 统一在这里定义，改配色只改这一处。
 * 深色底是为了配这块 TN 屏：它的可视角度差，深底比白底耐看。
 */
#define UI_C_BG		lv_color_hex(0x102040)	/* 屏幕底色 */
#define UI_C_BAR	lv_color_hex(0x1A2A4A)	/* 标题栏 / 状态条 */
#define UI_C_LIST	lv_color_hex(0x14243C)	/* 列表行底色 */
#define UI_C_LINE	lv_color_hex(0x2A3A5A)	/* 分割线 */
#define UI_C_TEXT	lv_color_hex(0xE0E8F0)	/* 主文字 */
#define UI_C_DIM	lv_color_hex(0x88A0C0)	/* 次要文字 */
#define UI_C_ACCENT	lv_color_hex(0x3AA0FF)	/* 强调色（选中/开关）*/

/* ---------- 全局设置 ----------
 * 多个页面要共享同一份状态：
 *   主页的音量项显示的值  ←→ 音量页的滑块
 * 所以不能各自存一份，必须有单一数据源。
 * 真实产品里这个结构会写到 flash / 配置文件；这里先放内存。
 */
struct ui_settings {
	bool	wake_word;	/* 唤醒词 */
	bool	voice;		/* 语音播报 */
	bool	auto_bright;	/* 自动调节亮度 */
	bool	camera;		/* 摄像头 */
	int	volume;		/* 音量 0~100 */
};

extern struct ui_settings g_settings;

/* ---------- 页面导航 ----------
 * 页面的创建函数都带缓存：第二次调用直接返回已有对象，
 * 不会重建（重建会丢掉控件状态，还浪费内存和时间）。
 */
lv_obj_t *ui_page_home(void);		/* 主页（导航栈底）*/
lv_obj_t *ui_page_net(void);		/* 网络连接 */
lv_obj_t *ui_page_volume(void);		/* 音量 */
lv_obj_t *ui_page_about(void);		/* 固件版本 */
lv_obj_t *ui_page_netcheck(void);	/* 网络自检（五层诊断）*/
lv_obj_t *ui_page_camera(void);		/* 摄像头：预览 + 拍照 + 录像 */
lv_obj_t *ui_page_motortest(void);	/* 电机：两路 H 桥正/反/停 + 全部停止 */
lv_obj_t *ui_page_yolo(void);		/* YOLO 相机：实时检测框（NPU）*/
lv_obj_t *ui_page_aichat(void);		/* AI 对话：语音链路入口 */
lv_obj_t *ui_page_brightness(void);	/* 屏幕亮度：滑块 + 快捷档 */
lv_obj_t *ui_page_avatar(void);		/* 桌面形象：多角色可选 */

/* 形象控件：按当前角色画一个 S x S 的头像，返回容器（可挂事件/自己 align） */
lv_obj_t *ui_avatar_create(lv_obj_t *parent, int size);

/* 换角色后调用：把所有已存在的形象重画（主页那个也要跟着变） */
void ui_avatar_refresh_all(void);

/* 摄像头是否正在录像（自测用）。stdbool 已在文件开头 include */
bool ui_camera_recording(void);

/* 在 parent 里放一张天气卡片（异步拉数据，10 分钟自动刷新）。
 * 返回卡片对象。实现见 ui_weathercard.c。 */
lv_obj_t *ui_weather_card(lv_obj_t *parent);

void ui_nav_init(lv_obj_t *root);	/* 登记根页面，必须在跳转前调用 */
void ui_nav_push(lv_obj_t *scr);	/* 进入子页：新页从右边推进来 */
void ui_nav_back(void);			/* 返回上一页：当前页往右退出去 */

/* 调试用：按名字直接打开某个页面，不经过点击。
 * 名字取值 "home" / "net" / "volume" / "about" / "netcheck" / "camera" / "motor"。
 * 返回 0 表示成功，-1 表示名字不认识。
 * 用途：改完 UI 想单独看某个子页长什么样，不用每次都
 * 先开主页、再精确点到那一行 —— 板子上用手指点准很难。 */
int ui_goto(const char *name);

/* ---------- 公共控件 ---------- */

/**
 * 界面统一字体：中文字模 + 符号 fallback。
 *
 * 我们的字模只覆盖 ASCII / 全角标点 / 汉字，而 LVGL 内置符号
 * （LV_SYMBOL_OK、BACKSPACE 等）在 Unicode 私用区 0xF000+，
 * 不在字模里 → 键盘的功能键会显示空白。
 * 这个字体把 fallback 挂到内置 montserrat_14（含这些符号）。
 *
 * 用法：给 screen 或 layer_top 调一次 ui_apply_font()，
 * 子对象靠样式继承自动生效。
 */
const lv_font_t *ui_font(void);
void ui_apply_font(lv_obj_t *obj);

lv_obj_t *ui_make_screen(const char *title, bool with_back);
lv_obj_t *ui_screen_body(lv_obj_t *scr);

/* 造一个设置行。
 * bind_field 非 NULL → 右侧放开关，初值取 *bind_field，切换写回它；
 *               NULL → 右侧放 ">" 箭头，点击整行走 cb。
 */
lv_obj_t *ui_make_row(lv_obj_t *parent, const char *name, bool *bind_field,
		      lv_event_cb_t cb, void *user_data);

/* 主页把它的状态条 label 登记进来，之后任何人调 ui_status 都写到那上面 */
void ui_status_bind(lv_obj_t *label);

/* 往状态条写一行字（printf 风格）。无绑定对象时静默忽略。 */
void ui_status(const char *fmt, ...);

/* 自动回归测试：不需要人按屏幕，用 lv_obj_send_event 走完整回调链，
 * 打印 PASS/FAIL 汇总。由环境变量 LVGL_SELFTEST=1 触发。 */
void ui_selftest(void);

/* 弹出密码输入框（全屏遮罩 + 键盘）。
 * on_done(ssid, psk)：点"确定"时 psk 是输入的密码；点"取消"/"X" 时 psk 为 NULL。
 * 实现见 ui_wifidlg.c。 */
void ui_wifi_psk_dialog(const char *ssid,
			void (*on_done)(const char *ssid, const char *psk));

/* 调试：不连任何网络，直接弹键盘看布局 */
void ui_wifi_psk_dialog_demo(void);

/* 当前弹窗的目标 SSID（没弹窗时返回空串）。
 * 自测用：验证"点击 AP 行 → 弹窗拿到的 SSID 正确"，
 * 即验证事件 user_data 没有踩悬垂指针。 */
const char *ui_wifi_dialog_current_ssid(void);

/* 强制关闭弹窗（不触发回调），自测清理现场用。 */
void ui_wifi_dialog_abort(void);

#endif /* __UI_H__ */
