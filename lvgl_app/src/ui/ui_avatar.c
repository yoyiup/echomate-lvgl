/**
 * ui_avatar.c —— 桌面形象：绘制 + 角色选择页
 *
 * 【形象怎么画】
 *   全部用基础图形拼：脸 = 圆角矩形（圆角=半径/2 就是圆），
 *   眼睛 = 小圆角矩形，嘴 = 短圆角条，耳朵按造型加装饰。
 *   不用图片素材，换角色只换颜色和造型参数（数据在 avatar.cpp 的表里）。
 *
 * 【眨眼怎么做】
 *   一个全局 lv_timer，每 2.2 秒把所有登记过的眼睛压扁成一条缝，
 *   120ms 后再撑回来。
 *   为什么用"登记表"而不是每个控件一个 timer：页面上可能同时存在
 *   主页的小头像 + 选择页的四个预览，5 个 timer 各跑各的没必要，
 *   而且对象销毁后忘了删 timer 就是野指针 —— LVGL 里最典型的一类崩溃。
 *   这里在 LV_EVENT_DELETE 里把登记项清掉。
 */
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "../avatar/avatar.h"
#include "../aichat/voice_chat.h"	/* 只读它那份线程安全快照，用来决定表情 */

#define AV_MAX		6	/* 同时存在的形象控件上限 */

static struct {
	lv_obj_t *root;
	lv_obj_t *eye_l;
	lv_obj_t *eye_r;
	lv_obj_t *mouth;
	lv_obj_t *body;		/* 装所有图形的内层容器，浮动时整体平移它 */
	int	 eye_w, eye_h;		/* 眼睛原始尺寸 */
	int	 mouth_w, mouth_h;	/* 嘴原始尺寸 */
	int	 base_ty;		/* 浮动基准（每个控件自己的） */
} g_reg[AV_MAX];

static lv_timer_t *g_blink_timer;

/* 点击反馈的截止时刻（lv_tick 毫秒）。0 表示没有反馈在进行。
 * 用"截止时刻"而不是布尔量：连点几次只会刷新这一刻，不会叠加出
 * 一串永远关不掉的动画。 */
static uint32_t g_react_until;

/* ------------------------------------------------------------------ */
/* 登记表                                                             */
/* ------------------------------------------------------------------ */

static int reg_slot_free(void)
{
	for (int i = 0; i < AV_MAX; i++)
		if (!g_reg[i].root)
			return i;
	return -1;
}

static void reg_clear(lv_obj_t *root)
{
	for (int i = 0; i < AV_MAX; i++)
		if (g_reg[i].root == root)
			memset(&g_reg[i], 0, sizeof(g_reg[i]));
}

static int reg_find(lv_obj_t *root)
{
	for (int i = 0; i < AV_MAX; i++)
		if (g_reg[i].root == root)
			return i;
	return -1;
}

/* 把所有的眼睛设成"睁"或"闭" */
static void set_eyes_closed(int closed)
{
	for (int i = 0; i < AV_MAX; i++) {
		if (!g_reg[i].root || !g_reg[i].eye_l)
			continue;

		int h = closed ? 2 : g_reg[i].eye_h;

		lv_obj_set_height(g_reg[i].eye_l, h);
		lv_obj_set_height(g_reg[i].eye_r, h);
	}
}

static void blink_open_cb(lv_timer_t *t)
{
	set_eyes_closed(0);
	lv_timer_delete(t);		/* 一次性，睁眼就把自己删了 */
}

/*
 * 眨眼周期。
 *
 * ★ 第一版是"每 2.2 秒翻转一次状态"，结果眼睛闭着整整 2.2 秒才睁开，
 *   看起来像没睡醒/笑眯眼，根本不像眨眼。
 *   真人眨眼闭合只有 0.1~0.2 秒，所以这里改成：
 *   定时器触发时先闭眼，再挂一个 130ms 的一次性定时器把眼睛睁开。
 */
static void blink_tick(lv_timer_t *t)
{
	(void)t;

	set_eyes_closed(1);
	lv_timer_create(blink_open_cb, 130, NULL);
}

/* ------------------------------------------------------------------ */
/* 情绪动画                                                           */
/* ------------------------------------------------------------------ */

/*
 * 表情从语音链路的状态推出来，不额外发明状态机：
 *   空闲/未启动 → 轻微上下浮动
 *   聆听中     → 嘴收成小圆点（像在听）
 *   思考中     → 嘴抿成一条线
 *   说话中     → 嘴一开一合
 *
 * 【为什么读 voice_chat_get() 而不让 aichat 反向调用】
 *   那样要跨模块回调，还得考虑线程；这里拿的是一份 atomic 快照，
 *   任何线程任何频率都能读，UI 层直接问就行。
 *   链路没起来时 state 是 FAULT，落到"空闲"分支，不会乱动。
 */
enum { MOOD_IDLE, MOOD_LISTEN, MOOD_THINK, MOOD_SPEAK };

static int current_mood(void)
{
	voice_chat_snap_t snap;

	voice_chat_get(&snap);

	switch (snap.state) {
	case VC_STATE_LISTENING: return MOOD_LISTEN;
	case VC_STATE_THINKING:  return MOOD_THINK;
	case VC_STATE_SPEAKING:  return MOOD_SPEAK;
	default:                 return MOOD_IDLE;
	}
}

static void anim_tick(lv_timer_t *t)
{
	static int phase;
	const int mood = current_mood();

	(void)t;
	phase++;

	/* 被点了一下：不管当前什么状态，先给个反应 */
	const bool react = (int32_t)(g_react_until - lv_tick_get()) > 0;

	for (int i = 0; i < AV_MAX; i++) {
		if (!g_reg[i].root || !g_reg[i].mouth)
			continue;

		int ty = 0;		/* 整个形象的上下浮动 */

		/* 先把嘴恢复成原始宽度，再按情绪改高度 —— 否则从"聆听"
		 * 切到别的状态时嘴会一直是圆的 */
		lv_obj_set_width(g_reg[i].mouth, g_reg[i].mouth_w);

		if (react) {
			/* 眯眼 + 张大嘴 + 蹦一下。
			 * 蹦的动作靠每 120ms 在 0/-2 之间切换，2 拍一个来回。 */
			lv_obj_set_height(g_reg[i].eye_l, 2);
			lv_obj_set_height(g_reg[i].eye_r, 2);
			lv_obj_set_width(g_reg[i].mouth,
					 g_reg[i].mouth_w * 4 / 5);
			lv_obj_set_height(g_reg[i].mouth,
					 g_reg[i].mouth_h * 5 / 2);
			lv_obj_set_y(g_reg[i].body, (phase % 2) ? -2 : 0);
			continue;
		}

		/* 反应结束后把眼睛恢复（眨眼定时器不一定会立刻补上） */
		lv_obj_set_height(g_reg[i].eye_l, g_reg[i].eye_h);
		lv_obj_set_height(g_reg[i].eye_r, g_reg[i].eye_h);

		switch (mood) {
		case MOOD_SPEAK: {
			/* 4 拍一个循环：张嘴 2 拍、闭嘴 2 拍 */
			const int open = (phase % 4) < 2;

			lv_obj_set_height(g_reg[i].mouth,
					  open ? g_reg[i].mouth_h * 2
					       : g_reg[i].mouth_h / 2 + 2);
			break;
		}
		case MOOD_LISTEN:
			/* 嘴收成小圆点：宽高相等 + 全圆角 */
			lv_obj_set_width(g_reg[i].mouth, g_reg[i].mouth_h * 2);
			lv_obj_set_height(g_reg[i].mouth, g_reg[i].mouth_h * 2);
			break;
		case MOOD_THINK:
			lv_obj_set_height(g_reg[i].mouth, 2);
			break;
		default:
			lv_obj_set_height(g_reg[i].mouth, g_reg[i].mouth_h);
			/* 空闲时慢慢上下浮动 1px，8 拍一个来回 */
			ty = ((phase / 8) % 2) ? -1 : 0;
			break;
		}

		if (g_reg[i].body)
			lv_obj_set_y(g_reg[i].body, ty);
	}
}

static void ensure_blink_timer(void)
{
	if (!g_blink_timer) {
		g_blink_timer = lv_timer_create(blink_tick, 2200, NULL);
		/* 动画单独一个定时器：眨眼是"事件"，表情是"持续状态"，
		 * 混在一起会让眨眼节奏被表情刷新带偏。 */
		lv_timer_create(anim_tick, 120, NULL);
	}
}

/* ------------------------------------------------------------------ */
/* 绘制                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *mk_shape(lv_obj_t *parent, int w, int h, uint32_t color,
			  int radius)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, radius, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);

	return o;
}

/*
 * 画一个形象。S = 外框边长，所有尺寸按比例算，
 * 所以同一个函数既能画主页的 72px 小头像，也能画选择页的预览。
 */
static void draw_one(lv_obj_t *root, const struct avatar_style *st, int S)
{
	const int fs = S * 82 / 100;		/* 脸直径 */
	const int ear = S * 26 / 100;		/* 耳朵直径 */
	const int eye_w = S * 15 / 100;
	const int eye_h = S * 19 / 100;
	const int mouth_w = S * 24 / 100;
	const int mouth_h = S * 7 / 100;

	/*
	 * ★ 所有图形挂在一个内层容器上，而不是直接挂 root。
	 *
	 *   原因：lv_obj_set_style_translate_y() 只平移【对象自己】的绘制，
	 *   不会带着子对象一起动 —— 第一版就是直接平移 root，
	 *   结果背景（透明的）挪了，脸和眼睛一动不动，实测头像区域
	 *   两帧差异 0 像素。
	 *   而【改对象的坐标】是另一回事：子对象的坐标是相对父对象的，
	 *   父对象一挪，子对象整体跟着走。所以浮动要平移这个 container。
	 */
	lv_obj_t *body = lv_obj_create(root);

	lv_obj_remove_style_all(body);
	lv_obj_set_size(body, S, S);
	lv_obj_set_pos(body, 0, 0);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

	/* ---- 装饰先画，脸后画，脸会盖住装饰的下半部分 ---- */
	switch (st->ear) {
	case AV_EAR_CAT: {
		lv_obj_t *l = mk_shape(body, ear, ear, st->ear_color, ear / 2);
		lv_obj_t *r = mk_shape(body, ear, ear, st->ear_color, ear / 2);

		lv_obj_align(l, LV_ALIGN_TOP_LEFT, S * 14 / 100, S * 6 / 100);
		lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -S * 14 / 100, S * 6 / 100);
		break;
	}
	case AV_EAR_ANTENNA: {
		lv_obj_t *rod = mk_shape(body, S * 4 / 100, S * 22 / 100,
					 st->ear_color, 1);
		lv_obj_t *ball = mk_shape(body, S * 13 / 100, S * 13 / 100,
					  st->ear_color, S * 7 / 100);

		lv_obj_align(rod, LV_ALIGN_TOP_MID, 0, S * 4 / 100);
		lv_obj_align(ball, LV_ALIGN_TOP_MID, 0, 0);
		break;
	}
	case AV_EAR_LEAF: {
		/* 两片叶子：用小圆角矩形当叶形，靠角度区分朝向 */
		lv_obj_t *l = mk_shape(body, ear * 2 / 3, ear,
					st->ear_color, ear / 2);
		lv_obj_t *r = mk_shape(body, ear * 2 / 3, ear,
					st->ear_color, ear / 2);

		lv_obj_align(l, LV_ALIGN_TOP_LEFT, S * 20 / 100, S * 2 / 100);
		lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -S * 20 / 100, S * 2 / 100);
		break;
	}
	default:
		break;			/* AV_EAR_NONE */
	}

	/* ---- 脸 ---- */
	lv_obj_t *face = mk_shape(body, fs, fs, st->face, fs / 2);

	lv_obj_center(face);

	/* ---- 腮红 ---- */
	lv_obj_t *cl = mk_shape(body, S * 16 / 100, S * 10 / 100, st->cheek,
				S * 5 / 100);
	lv_obj_t *cr = mk_shape(body, S * 16 / 100, S * 10 / 100, st->cheek,
				S * 5 / 100);

	lv_obj_align(cl, LV_ALIGN_CENTER, -fs / 3, fs / 6);
	lv_obj_align(cr, LV_ALIGN_CENTER, fs / 3, fs / 6);

	/* ---- 眼睛（登记给眨眼定时器）---- */
	lv_obj_t *el = mk_shape(body, eye_w, eye_h, st->eye, eye_h / 2);
	lv_obj_t *er = mk_shape(body, eye_w, eye_h, st->eye, eye_h / 2);

	lv_obj_align(el, LV_ALIGN_CENTER, -fs / 5, -fs / 10);
	lv_obj_align(er, LV_ALIGN_CENTER, fs / 5, -fs / 10);

	/* ---- 嘴 ---- */
	lv_obj_t *mo = mk_shape(body, mouth_w, mouth_h, st->mouth, mouth_h / 2);

	lv_obj_align(mo, LV_ALIGN_CENTER, 0, fs / 5);

	int slot = reg_find(root);

	if (slot < 0)
		slot = reg_slot_free();
	if (slot >= 0) {
		g_reg[slot].root    = root;
		g_reg[slot].eye_l   = el;
		g_reg[slot].eye_r   = er;
		g_reg[slot].mouth   = mo;
		g_reg[slot].body    = body;
		g_reg[slot].eye_w   = eye_w;
		g_reg[slot].eye_h   = eye_h;
		g_reg[slot].mouth_w = mouth_w;
		g_reg[slot].mouth_h = mouth_h;
		g_reg[slot].base_ty = 0;
	}
}

static void avatar_click_cb(lv_event_t *e)
{
	(void)e;
	g_react_until = lv_tick_get() + 420;	/* 反馈持续 420ms */
}

/*
 * 调试开关 AVATAR_DEMO_TAP=1：每 2 秒给头像发一次 CLICKED。
 * 用途：没法用手指点的时候，也能验证"点击反馈"这套动画是不是真的在跑
 * （抓帧比像素就能看出来）。不设这个变量时完全不注册这个定时器。
 */
static void demo_tap_cb(lv_timer_t *t)
{
	lv_obj_send_event((lv_obj_t *)lv_timer_get_user_data(t),
			  LV_EVENT_CLICKED, NULL);
}

static void avatar_delete_cb(lv_event_t *e)
{
	reg_clear(lv_event_get_target(e));
}

/* 把控件里的旧内容全删了重画（换角色时用） */
static void redraw(lv_obj_t *root, int S)
{
	const struct avatar_ops *A = avatar_api();
	const struct avatar_style *st = A->style(A->get());

	lv_obj_clean(root);
	reg_clear(root);

	if (!st)
		return;

	draw_one(root, st, S);
}

/* ------------------------------------------------------------------ */
/* 对外：创建形象控件                                                 */
/* ------------------------------------------------------------------ */

/*
 * 建一个形象控件。
 *   id < 0  → 画"当前选中"的角色（主页那个用）
 *   id >= 0 → 画指定的角色（选择页的四个预览用）
 *
 * ★ 之前只有一个不带 id 的版本，选择页四个预览全画成了当前角色 ——
 *   因为绘制时都去取 avatar_api()->get()。这种"预览全是同一个"的 bug
 *   不会报错，只有盯着屏幕才看得出来。
 */
static lv_obj_t *avatar_make(lv_obj_t *parent, int size, int id)
{
	const struct avatar_ops *A = avatar_api();
	const struct avatar_style *st;
	lv_obj_t *box = lv_obj_create(parent);

	if (id < 0)
		id = A->get();
	st = A->style(id);

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, size, size);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	/* 可点：摸一下会有反应（睁大眼/跳一下） */
	lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(box, avatar_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_add_event_cb(box, avatar_delete_cb, LV_EVENT_DELETE, NULL);

	if (getenv("AVATAR_DEMO_TAP"))
		lv_timer_create(demo_tap_cb, 2000, box);

	lv_obj_set_user_data(box, (void *)(intptr_t)size);

	if (st)
		draw_one(box, st, size);

	ensure_blink_timer();

	return box;
}

lv_obj_t *ui_avatar_create(lv_obj_t *parent, int size)
{
	return avatar_make(parent, size, -1);
}

/* 换角色后，把所有已登记的形象重画一遍（主页那个也要跟着变） */
void ui_avatar_refresh_all(void)
{
	for (int i = 0; i < AV_MAX; i++) {
		if (!g_reg[i].root)
			continue;

		int S = (int)(intptr_t)lv_obj_get_user_data(g_reg[i].root);

		if (S > 0)
			redraw(g_reg[i].root, S);
	}
}

/* ------------------------------------------------------------------ */
/* 选择页                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *page_avatar;

static void pick_cb(lv_event_t *e)
{
	int id = (int)(intptr_t)lv_event_get_user_data(e);

	avatar_api()->set(id);
	ui_avatar_refresh_all();
	ui_goto("home");
}

static lv_obj_t *build_avatar_page(void)
{
	const struct avatar_ops *A = avatar_api();
	const int n = A->count();
	const int cur = A->get();

	lv_obj_t *scr = ui_make_screen("桌面形象", true);
	lv_obj_t *body = ui_screen_body(scr);

	lv_obj_set_style_pad_all(body, 8, 0);
	lv_obj_set_style_pad_row(body, 8, 0);
	lv_obj_set_style_pad_column(body, 8, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* 2x2 卡片：每张 148x88，正好铺满 320 宽 */
	for (int i = 0; i < n && i < 4; i++) {
		const struct avatar_style *st = A->style(i);
		lv_obj_t *card = lv_obj_create(body);

		lv_obj_set_size(card, 148, 86);
		lv_obj_set_style_bg_color(card, UI_C_BAR, 0);
		lv_obj_set_style_border_width(card, i == cur ? 2 : 0, 0);
		lv_obj_set_style_border_color(card, UI_C_ACCENT, 0);
		lv_obj_set_style_radius(card, 8, 0);
		lv_obj_set_style_pad_all(card, 4, 0);
		lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(card, pick_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)i);

		/* 左边预览 60x60，右边名字 */
		lv_obj_t *preview = avatar_make(card, 60, i);

		lv_obj_align(preview, LV_ALIGN_LEFT_MID, 2, 0);

		lv_obj_t *nm = lv_label_create(card);

		lv_label_set_text(nm, st ? st->name : "?");
		lv_obj_set_style_text_color(nm, UI_C_TEXT, 0);
		lv_obj_align(nm, LV_ALIGN_TOP_RIGHT, -4, 22);
	}

	return scr;
}

lv_obj_t *ui_page_avatar(void)
{
	if (!page_avatar)
		page_avatar = build_avatar_page();
	return page_avatar;
}
