/**
 * ui_brightness.c —— 屏幕亮度调节页
 *
 * 【布局】
 *   ┌──────────────────────────────┐
 *   │ 屏幕亮度                 返回 │ 34
 *   ├──────────────────────────────┤
 *   │            50                 │ 大号数字（缩放 2 倍绘制）
 *   │        亮度 0 - 99            │
 *   │      [========|--------]      │ 滑块
 *   │    [暗]    [中]    [亮]       │ 快捷档
 *   └──────────────────────────────┘
 *
 * 【为什么加"快捷档"】
 *   这块屏是电阻/电容触摸，滑块拖到精确位置很难（320 宽里 1 档≈2.8px）。
 *   给三个常用档位比让人死磕滑块实用得多。
 *
 * 【为什么滑块变化就立刻写 sysfs】
 *   背光调节必须所见即所得，松手才生效会让人以为没调上。
 *   写 sysfs 是一次几十微秒的 write，滑动过程中的事件频率完全扛得住。
 */

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "../bright/brightness.h"

/* 背光能力的函数表（bright_api() 返回的那张） */
static const struct bright_ops *B;

static lv_obj_t *page_bright;
static lv_obj_t *val_lb;
static lv_obj_t *slider;

static void update_label(int v)
{
	if (!val_lb)
		return;
	lv_label_set_text_fmt(val_lb, "%d", v);
}

static void slider_cb(lv_event_t *e)
{
	int v = (int)lv_slider_get_value(lv_event_get_target(e));

	update_label(v);
	if (B->set(v) < 0)
		lv_label_set_text(val_lb, "ERR");
}

/* 快捷档：暗 / 中 / 亮，按 max 比例算，换屏也不用改代码 */
static void preset_cb(lv_event_t *e)
{
	int pct = (int)(intptr_t)lv_event_get_user_data(e);
	int mx = B->max();
	int v = mx * pct / 100;

	if (v < BRIGHT_MIN)
		v = BRIGHT_MIN;

	B->set(v);
	if (slider)
		lv_slider_set_value(slider, v, LV_ANIM_OFF);
	update_label(v);
}

static lv_obj_t *make_preset(lv_obj_t *parent, const char *txt, int pct)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_height(b, 34);
	lv_obj_set_style_bg_color(b, UI_C_LINE, 0);
	lv_obj_set_style_radius(b, 6, 0);
	lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pct);

	lv_obj_t *l = lv_label_create(b);

	lv_label_set_text(l, txt);
	lv_obj_center(l);

	return b;
}

static lv_obj_t *build_brightness(void)
{
	B = bright_api();	/* 拿函数表 */

	lv_obj_t *scr = ui_make_screen("屏幕亮度", true);
	lv_obj_t *body = ui_screen_body(scr);
	int cur = B->get();
	int mx = B->max();

	if (cur < 0)
		cur = mx / 2;

	/*
	 * ★ 自愈：进页面时如果当前亮度已经低于下限（比如以前被拖到 5），
	 *   直接写回 BRIGHT_MIN。
	 *   只"显示"成 10 而不写回去是不行的 —— 屏幕还是那么暗，
	 *   用户看不清楚，也没意识到需要动一下滑块。
	 */
	if (cur < BRIGHT_MIN) {
		B->set(BRIGHT_MIN);
		cur = BRIGHT_MIN;
	}

	/* ---- 大号数字 ---- */
	val_lb = lv_label_create(body);
	lv_obj_set_style_text_color(val_lb, UI_C_ACCENT, 0);
	lv_obj_set_style_transform_scale(val_lb, 512, 0);	/* 2 倍绘制 */
	lv_obj_add_flag(val_lb, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(val_lb, LV_ALIGN_TOP_MID, 0, 14);
	update_label(cur);

	/* ---- 量程提示 ---- */
	lv_obj_t *unit = lv_label_create(body);

	lv_label_set_text_fmt(unit, "亮度 %d - %d", BRIGHT_MIN, mx);
	lv_obj_set_style_text_color(unit, UI_C_DIM, 0);
	lv_obj_add_flag(unit, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 56);

	/* ---- 滑块 ---- */
	slider = lv_slider_create(body);
	lv_obj_set_size(slider, 280, 12);
	lv_slider_set_range(slider, BRIGHT_MIN, mx);
	lv_slider_set_value(slider, cur, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(slider, UI_C_LINE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(slider, UI_C_ACCENT, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(slider, UI_C_ACCENT, LV_PART_KNOB);
	lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_flag(slider, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 86);

	/* ---- 三个快捷档 ---- */
	lv_obj_t *btns = lv_obj_create(body);

	lv_obj_set_size(btns, 300, 40);
	lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btns, 0, 0);
	lv_obj_set_style_pad_all(btns, 0, 0);
	lv_obj_set_style_pad_column(btns, 6, 0);
	lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(btns, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(btns, LV_ALIGN_TOP_MID, 0, 120);

	make_preset(btns, "暗", 25);
	make_preset(btns, "中", 50);
	make_preset(btns, "亮", 90);

	/* ---- 设备不可用时的提示 ---- */
	if (!B->available()) {
		lv_obj_t *w = lv_label_create(body);

		lv_label_set_text(w, "没有背光设备，调节无效");
		lv_obj_set_style_text_color(w, lv_color_hex(0xFF6060), 0);
		lv_obj_add_flag(w, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_align(w, LV_ALIGN_BOTTOM_MID, 0, -10);
	}

	return scr;
}

lv_obj_t *ui_page_brightness(void)
{
	if (!page_bright)
		page_bright = build_brightness();
	return page_bright;
}
