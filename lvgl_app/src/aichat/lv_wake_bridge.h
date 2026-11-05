// LVGL 集成桥接层 —— C 接口（main.c 直接调用）
//
// ★ 唯一硬约束：LVGL 是单线程的，lv_obj_* 只能在主线程（调 lv_timer_handler 的那个）
//   里操作。唤醒框架的业务线程绝不能直接碰 UI，否则是随机崩溃/花屏。
//
// 本桥接层做两件事：
//   ① 业务线程只写 std::atomic 快照（wake_runtime 内部已经是 atomic）
//   ② UI 线程用 lv_timer 每 100ms 拉一次，变了才刷 label
//   —— 和你项目里 WiFi 扫描"用 lv_timer 查 seq 变化才重建列表"是同一个套路。
#ifndef LV_WAKE_BRIDGE_H
#define LV_WAKE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 参数用 void* 而不是 lv_obj_t*：
//   C++ 里前向声明 `struct _lv_obj_t` 会和 lvgl 的
//   `typedef struct _lv_obj_t lv_obj_t;` 打架（编译器视为两个类型，
//   报 conflicting declaration）。用 void* 让 C/C++ 都隐式转换，零冲突。
//   调用方照常传 lv_obj_t*，不用改。

// 启动唤醒框架（内部起业务线程 + 守护线程）。幂等。
void wake_bridge_init(void);

// 绑定一个 label 用于显示状态。内部创建 100ms 的 lv_timer。
// ★ 必须在 UI 创建之后、且在主线程调用。
void wake_bridge_attach(void* label);

// 喂一帧音频（模式 A 的 SDK 需要；模式 B 自开麦就不用调）
void wake_bridge_feed(const int16_t* pcm, size_t samples);

// 模拟按键唤醒（语音降级后的兜底入口，走同一个事件/状态机）
void wake_bridge_press(void);

// 取一行状态文本（调试/日志用，主线程调用）
const char* wake_bridge_status(void);

// 关闭：先停 SDK，再停业务线程，最后删 timer
void wake_bridge_deinit(void);

/* ---------- 对话链路的显式启停（「AI 对话」页面用） ----------
 *
 * 改成"界面入口"之后，链路不再开机自动拉起，而是由页面按钮触发：
 *   进页面 → wake_bridge_chat_start()
 *   退页面 → wake_bridge_chat_stop()
 *
 * 为什么要收口到这里、不让页面直接调 voice_chat_*：
 *   voice_chat_* 不知道当前是不是演示模式(WAKE_MODE=mock/own/storm)。
 *   演示模式下再拉真链路会两路抢麦克风，谁都收不到声音。
 */
int  wake_bridge_chat_start(void);   // 0 成功；-1 失败/演示模式
void wake_bridge_chat_stop(void);
int  wake_bridge_chat_running(void); // 0/1

/* 主动唤醒一次（等价于喊唤醒词）。
 * echo.pmdl 是英文个人模型，对中文口音不稳，界面上的「唤醒」按钮走这里兜底。 */
int  wake_bridge_chat_wake(void);

#ifdef __cplusplus
}
#endif

#endif // LV_WAKE_BRIDGE_H
