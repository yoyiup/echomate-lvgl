#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "../wifi/hotspot_control.h"

static lv_obj_t *page_hotspot;
static lv_obj_t *state_lb;
static lv_obj_t *ssid_lb;
static lv_obj_t *password_lb;
static lv_obj_t *url_lb;
static lv_obj_t *note_lb;
static lv_obj_t *toggle_btn;
static lv_obj_t *toggle_lb;

static void set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text) != 0)
		lv_label_set_text(label, text);
}

static void refresh(void)
{
	hotspot_info info;
	char password_text[64];
	static int last_running = -1;
	hotspot_get(&info);
	if (info.busy == 1)
		set_text(state_lb, "正在开启热点…");
	else if (info.busy == 2)
		set_text(state_lb, "正在关闭热点…");
	else if (info.last_error)
		set_text(state_lb, "切换失败，请检查服务");
	else
		set_text(state_lb, info.running ? "热点已开启" : "热点已关闭");
	if (last_running != info.running) {
		lv_obj_set_style_text_color(state_lb,
			info.running ? lv_color_hex(0x5FD08A) : UI_C_TEXT, 0);
		last_running = info.running;
	}
	set_text(ssid_lb, "名称：EchoMate-48B4");
	if (info.running && info.password[0])
		snprintf(password_text, sizeof(password_text), "密码：%s", info.password);
	else
		snprintf(password_text, sizeof(password_text), "密码：开启后显示");
	set_text(password_lb, password_text);
	set_text(url_lb, "手机浏览器：192.168.50.1:8080");
	set_text(note_lb, "上传后可在网页下载；重启会清空。\n热点开启时，原 Wi-Fi 暂停使用。");
	set_text(toggle_lb, info.running ? "关闭热点" : "开启热点");
	if (info.busy)
		lv_obj_add_state(toggle_btn, LV_STATE_DISABLED);
	else
		lv_obj_remove_state(toggle_btn, LV_STATE_DISABLED);
}

static void toggle_cb(lv_event_t *e)
{
	(void)e;
	if (hotspot_request(!hotspot_is_running()) != 0)
		ui_status("热点正在切换");
	refresh();
}

static void poll_cb(lv_timer_t *t)
{
	(void)t;
	if (lv_screen_active() == page_hotspot)
		refresh();
}

static void load_cb(lv_event_t *e)
{
	(void)e;
	refresh();
}

static lv_obj_t *build_page(void)
{
	lv_obj_t *scr = ui_make_screen("手机传文件", true);
	lv_obj_t *body = ui_screen_body(scr);
	lv_obj_set_style_pad_all(body, 8, 0);
	lv_obj_set_style_pad_row(body, 5, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	state_lb = lv_label_create(body);
	lv_obj_set_style_text_color(state_lb, UI_C_TEXT, 0);
	ssid_lb = lv_label_create(body);
	lv_obj_set_style_text_color(ssid_lb, UI_C_TEXT, 0);
	password_lb = lv_label_create(body);
	lv_obj_set_style_text_color(password_lb, UI_C_TEXT, 0);
	url_lb = lv_label_create(body);
	lv_obj_set_style_text_color(url_lb, UI_C_ACCENT, 0);
	note_lb = lv_label_create(body);
	lv_obj_set_width(note_lb, lv_pct(100));
	lv_obj_set_style_text_color(note_lb, UI_C_DIM, 0);

	toggle_btn = lv_button_create(body);
	lv_obj_set_size(toggle_btn, lv_pct(100), 34);
	lv_obj_set_style_bg_color(toggle_btn, UI_C_ACCENT, 0);
	lv_obj_add_event_cb(toggle_btn, toggle_cb, LV_EVENT_CLICKED, NULL);
	toggle_lb = lv_label_create(toggle_btn);
	lv_obj_set_style_text_color(toggle_lb, lv_color_hex(0x0A1628), 0);
	lv_obj_center(toggle_lb);

	lv_obj_add_event_cb(scr, load_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_timer_create(poll_cb, 500, NULL);
	refresh();
	return scr;
}

lv_obj_t *ui_page_hotspot(void)
{
	if (!page_hotspot)
		page_hotspot = build_page();
	return page_hotspot;
}
