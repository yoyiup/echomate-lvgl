/**
 * ui_aichat.c —— 「AI 对话」页（语音对话链路的入口）
 *
 * 【为什么要做成"入口页"而不是开机自动拉链路】
 *   链路一跑起来就独占麦克风、连云端、跑 Opus 编解码，一直占着 CPU。
 *   做成页面入口之后：
 *     进页面 → 拉起链路（可以对着它说话）
 *     退页面 → 停链路，麦克风释放、云端断开
 *   和摄像头页"进页面才 cam_start、离开就 cam_stop"是同一个思路。
 *
 * 【状态从哪来】
 *   voice_chat_get() 给的是一份 atomic 快照 —— 对话链路在别的线程里跑，
 *   UI 线程只能读快照，绝不能去读它的内部状态（跨线程碰裸数据会随机崩）。
 *
 * 【为什么用 lv_timer 轮询而不是回调】
 *   和相机/WiFi 扫描一样：业务线程不知道 LVGL 存在，
 *   让它回调就要跨线程碰 lv_obj_*，那是 LVGL 的单线程大忌。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "../aichat/lv_wake_bridge.h"
#include "../aichat/voice_chat.h"

static lv_obj_t *page_aichat;
static lv_obj_t *state_lb;
static lv_obj_t *round_lb;
static lv_obj_t *asr_lb;
static lv_obj_t *btn_lb;
static lv_obj_t *btn;

static int ui_state;		/* 页面认为的链路状态：0 未启动 1 已启动 */

/* 状态码 → 中文。屏幕是 16px 中文字体，写中文没问题。 */
static const char *state_text(int s)
{
	switch (s) {
	case VC_STATE_STARTUP:   return "启动中…";
	case VC_STATE_IDLE:      return "空闲";
	case VC_STATE_LISTENING: return "聆听中…";
	case VC_STATE_THINKING:  return "思考中…";
	case VC_STATE_SPEAKING:  return "说话中…";
	case VC_STATE_STOPPING:  return "正在停止";
	case VC_STATE_FAULT:     return "连接异常";
	default:                 return "未知";
	}
}

static uint32_t state_color(int s)
{
	switch (s) {
	case VC_STATE_LISTENING: return 0x40E070;	/* 绿：可以说话 */
	case VC_STATE_THINKING:  return 0x3AA0FF;	/* 蓝：等云端 */
	case VC_STATE_SPEAKING:  return 0xFFC040;	/* 黄：它在说 */
	case VC_STATE_FAULT:     return 0xFF6060;	/* 红 */
	default:                 return 0xE0E8F0;
	}
}

static void refresh_btn(void)
{
	lv_label_set_text(btn_lb, ui_state ? "停止对话" : "开始对话");
	lv_obj_set_style_bg_color(btn, lv_color_hex(ui_state ? 0xC03030 : 0x2E7D32), 0);
}

static void update_ui(void)
{
	voice_chat_snap_t s;

	if (!ui_state) {
		lv_label_set_text(state_lb, "未启动");
		lv_obj_set_style_text_color(state_lb, UI_C_DIM, 0);
		lv_label_set_text(round_lb, "点下面的按钮开始对话");
		lv_label_set_text(asr_lb, "");
		return;
	}

	voice_chat_get(&s);

	/*
	 * ★ 链路线程死掉之后，快照会停在 STOPPING / FAULT —— 它自己不会回到
	 *   "未启动"。如果界面照抄这个状态，用户看到的就是永远"正在停止"，
	 *   按钮还是"停止对话"，点一下只是再调一次 stop，没有任何反馈。
	 *   所以这里把「异常态」当"没连上"处理：状态改文案、按钮变回可重连。
	 */
	if (s.state == VC_STATE_FAULT || s.state == VC_STATE_STOPPING) {
		ui_state = 0;
		refresh_btn();
		lv_label_set_text(state_lb, "未连接");
		lv_obj_set_style_text_color(state_lb, lv_color_hex(0xFF6060), 0);
		lv_label_set_text(round_lb, "网关没起？172.32.0.1:8000");
		lv_label_set_text(asr_lb, "");
		return;
	}

	lv_label_set_text(state_lb, state_text(s.state));
	lv_obj_set_style_text_color(state_lb,
				    lv_color_hex(state_color(s.state)), 0);

	lv_label_set_text_fmt(round_lb, "第 %u 轮", s.rounds);

	/* ASR 结果是别人线程写的 char 数组，拷出来再用 */
	char buf[96];

	snprintf(buf, sizeof(buf), "%s", s.asr);
	if (strcmp(lv_label_get_text(asr_lb), buf) != 0)
		lv_label_set_text(asr_lb, buf);
}

/* 手动唤醒：直接触发一次 wake_detected，后面走和语音唤醒完全相同的链路。
 * 唤醒词识别不出来时的兜底 —— 不让人对着设备干喊。 */
static void wake_cb(lv_event_t *e)
{
	(void)e;

	if (!ui_state) {
		lv_label_set_text(state_lb, "先点「开始对话」");
		return;
	}

	if (wake_bridge_chat_wake() != 0) {
		lv_label_set_text(state_lb, "唤醒失败（链路没起？）");
		lv_obj_set_style_text_color(state_lb, lv_color_hex(0xFF6060), 0);
		return;
	}

	lv_label_set_text(state_lb, "已唤醒，请说话…");
	lv_obj_set_style_text_color(state_lb, lv_color_hex(0x40E070), 0);
}

static void poll_cb(lv_timer_t *t)
{
	(void)t;
	update_ui();
}

/* 点按钮：在"起"和"停"之间切 */
static void toggle_cb(lv_event_t *e)
{
	(void)e;

	if (!ui_state) {
		if (wake_bridge_chat_start() == 0) {
			ui_state = 1;
			lv_label_set_text(asr_lb, "");
		} else {
			lv_label_set_text(state_lb, "启动失败");
			lv_obj_set_style_text_color(state_lb,
						    lv_color_hex(0xFF6060), 0);
		}
	} else {
		wake_bridge_chat_stop();
		ui_state = 0;
	}

	refresh_btn();
	update_ui();
}

/*
 * 进页面：自动拉起链路（入口语义）。
 * 想改成"必须手动点按钮"就把这里的调用去掉，其它不用动。
 */
/* 调试开关：AICHAT_AUTOWAKE=1 时，进页面 1.5 秒后自动唤醒一次。
 * 用途：验证"唤醒之后"的整条链路（ASR→LLM→TTS→形象说话动画），
 * 不用真的对着板子喊。和 LVGL_START_PAGE 一样属于调试口的开关，
 * 不设这个变量时行为完全不变。 */
static void autowake_cb(lv_timer_t *t)
{
	static int tries;
	voice_chat_snap_t snap;

	voice_chat_get(&snap);

	/*
	 * ★ 必须等它真的进 Idle 再唤醒。
	 *   第一版是"进页面 1.5 秒后直接调"，结果 startup 阶段链路还没就绪，
	 *   wake_detected 事件发出去没人接，被直接丢掉 ——
	 *   表现是"手动唤醒打了日志，但状态一直停在 Idle 不动"。
	 */
	if (snap.state == VC_STATE_IDLE || ++tries > 40) {
		lv_timer_delete(t);
		wake_bridge_chat_wake();
		return;
	}

	lv_timer_set_period(t, 300);	/* 没就绪就 300ms 后再看 */
	lv_timer_reset(t);
}

static void screen_load_cb(lv_event_t *e)
{
	(void)e;

	if (!ui_state && wake_bridge_chat_start() == 0)
		ui_state = 1;

	const char *aw = getenv("AICHAT_AUTOWAKE");

	if (aw && aw[0] == '1')
		lv_timer_create(autowake_cb, 1500, NULL);

	refresh_btn();
	update_ui();
}

/*
 * 退页面：停链路。
 * ★ 必须停 —— 不停的话麦克风一直被占着，
 *   别的程序（arecord 测试、下次进来）都拿不到音频设备。
 */
static void screen_unload_cb(lv_event_t *e)
{
	(void)e;

	if (ui_state) {
		wake_bridge_chat_stop();
		ui_state = 0;
		refresh_btn();
	}
}

static lv_obj_t *build_aichat(void)
{
	lv_obj_t *scr = ui_make_screen("AI 对话", true);
	lv_obj_t *body = ui_screen_body(scr);

	lv_obj_set_style_pad_all(body, 8, 0);
	lv_obj_set_style_pad_row(body, 4, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	/* ---- 顶部：形象 + 状态 ----
	 *
	 * 形象放这里是为了让"说话时嘴动"这个动效真的能被看见 ——
	 * 对话发生在这个页面，如果只有主页有形象，人根本看不到表情变化。
	 * 左边 56x56，右边状态/轮次两行。
	 */
	lv_obj_t *top = lv_obj_create(body);

	lv_obj_set_size(top, lv_pct(100), 58);
	lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_set_style_pad_column(top, 6, 0);
	lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	ui_avatar_create(top, 56);

	lv_obj_t *scol = lv_obj_create(top);

	lv_obj_set_flex_grow(scol, 1);
	lv_obj_set_height(scol, lv_pct(100));
	lv_obj_set_style_bg_opa(scol, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(scol, 0, 0);
	lv_obj_set_style_pad_all(scol, 0, 0);
	lv_obj_remove_flag(scol, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(scol, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scol, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	/* ---- 状态 ---- */
	state_lb = lv_label_create(scol);
	lv_obj_set_style_text_color(state_lb, UI_C_DIM, 0);
	lv_label_set_text(state_lb, "未启动");

	/* ---- 轮次 / 提示 ---- */
	round_lb = lv_label_create(scol);
	lv_obj_set_style_text_color(round_lb, UI_C_DIM, 0);
	lv_label_set_text(round_lb, "点下面的按钮开始对话");

	/* ---- 最近一次识别结果 ---- */
	asr_lb = lv_label_create(body);
	lv_obj_set_width(asr_lb, lv_pct(100));
	/* 吃掉中间的空白，把按钮顶到底部 —— 不然按钮悬在半空，看着像没排版好 */
	lv_obj_set_flex_grow(asr_lb, 1);
	lv_obj_set_style_text_color(asr_lb, UI_C_TEXT, 0);
	lv_label_set_long_mode(asr_lb, LV_LABEL_LONG_WRAP);
	lv_label_set_text(asr_lb, "");

	/* ---- 手动唤醒（唤醒词识别不出来时的兜底）---- */
	lv_obj_t *wbtn = lv_button_create(body);

	lv_obj_set_width(wbtn, lv_pct(100));
	lv_obj_set_height(wbtn, 40);
	lv_obj_set_style_bg_color(wbtn, lv_color_hex(0x3AA0FF), 0);
	lv_obj_set_style_radius(wbtn, 6, 0);
	lv_obj_add_event_cb(wbtn, wake_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *wlb = lv_label_create(wbtn);

	lv_label_set_text(wlb, "唤 醒（说 echo 也行）");
	lv_obj_center(wlb);

	/* ---- 按钮 ---- */
	btn = lv_button_create(body);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_height(btn, 46);
	lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E7D32), 0);
	lv_obj_set_style_radius(btn, 6, 0);
	lv_obj_add_event_cb(btn, toggle_cb, LV_EVENT_CLICKED, NULL);

	btn_lb = lv_label_create(btn);
	lv_label_set_text(btn_lb, "开始对话");
	lv_obj_center(btn_lb);

	/* ---- 生命周期 + 轮询 ---- */
	lv_obj_add_event_cb(scr, screen_load_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(scr, screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START,
			    NULL);
	lv_timer_create(poll_cb, 200, NULL);

	ui_state = 0;
	refresh_btn();

	return scr;
}

lv_obj_t *ui_page_aichat(void)
{
	if (!page_aichat)
		page_aichat = build_aichat();
	return page_aichat;
}
