/**
 * ui_motortest.c —— 电机测试页（D1 驱动 /dev/echo_motor 的手动验证入口）
 *
 * 【页面布局】（320x240，标题栏 34，body 206）
 *   ┌────────────────────────────────┐
 *   │ 电机测试                  返回 │ 34
 *   ├────────────────────────────────┤
 *   │ 状态：就绪  左=停 右=停        │ 26
 *   │ 左 [正转][反转][停止]          │ 64
 *   │ 右 [正转][反转][停止]          │ 64
 *   │ [        全部停止        ]     │ 46
 *   └────────────────────────────────┘
 *
 * 【为什么状态栏要"读回来"而不是只显示按了什么】
 *   ECHO_MOTOR_GET 才是驱动里真实存的方向。界面如果只信自己刚按下什么，
 *   一旦 ioctl 失败（驱动没装、参数被拒），画面显示"正转"而电机不动，
 *   就会误判成"硬件/接线问题"。所以每次动作后都 query 一次，
 *   驱动说什么就显示什么；失败就把 M->last_error() 原样打出来。
 *
 * 【为什么离开页面必须强制停】
 *   驱动是"设方向"语义，写一次就一直保持 —— 不是按住才转。
 *   用户从这页退出去看别的，电机不会自己停。
 *   所以挂 LV_EVENT_SCREEN_UNLOAD_START 把两个电机都归零。
 *   （这个页面被缓存、不会销毁，所以每次进入/离开事件都会正常触发。）
 */

#include <stdio.h>
#include <stdint.h>

#include "ui.h"
#include "../motor/motor_ctl.h"

/* 按钮宽度：320 - 左右内边距 12 - "左/右"标签 34 - 间距 3*4 ≈ 262 / 3 */
#define BTN_W		84
#define BTN_H		50
#define ROW_H		64
#define STAT_H		26

/* 电机能力的函数表（全模块唯一 C 入口 motor_api() 返回的那张） */
static const struct motor_ops *M;

static lv_obj_t	*page_motor;
static lv_obj_t	*stat_lb;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */
static const char *dir_name(int dir)
{
	switch (dir) {
	case ECHO_MOTOR_FWD:   return "正转";
	case ECHO_MOTOR_REV:   return "反转";
	case ECHO_MOTOR_BRAKE: return "制动";
	default:               return "停";
	}
}

/*
 * 从驱动读回真实状态并刷新状态栏。
 * 读失败就把错误原样显示 —— 用户能立刻区分"没按对"和"驱动没装"。
 */
static void refresh_status(void)
{
	int dirs[ECHO_MOTOR_MAX];
	char buf[96];

	if (M->query(dirs) < 0) {
		lv_label_set_text(stat_lb, M->last_error());
		lv_obj_set_style_text_color(stat_lb, lv_color_hex(0xFF6060), 0);
		return;
	}

	snprintf(buf, sizeof(buf), "就绪  左=%s  右=%s",
		 dir_name(dirs[ECHO_MOTOR_LEFT]),
		 dir_name(dirs[ECHO_MOTOR_RIGHT]));
	lv_label_set_text(stat_lb, buf);
	lv_obj_set_style_text_color(stat_lb, UI_C_DIM, 0);
}

/* ------------------------------------------------------------------ */
/* 回调                                                               */
/* ------------------------------------------------------------------ */

/* 6 个方向按钮共用一个回调：user_data 打包成 (id << 4) | dir */
static void motor_btn_cb(lv_event_t *e)
{
	long v = (long)(intptr_t)lv_event_get_user_data(e);
	int id  = (int)(v >> 4);
	int dir = (int)(v & 0xF);

	if (M->set(id, dir) < 0) {
		/* 错误文案已经在 M->last_error() 里，refresh 会显示出来 */
		refresh_status();
		return;
	}

	refresh_status();
}

static void stop_all_cb(lv_event_t *e)
{
	(void)e;
	M->stop_all();
	refresh_status();
}

/*
 * 离开页面 → 两个电机都滑行停止。
 * 用 UNLOAD_START 而不是 UNLOAD：前者在旧页还显示着的时候就发出，
 * 手感更自然（用户已经在看新页了，不会看到"停了"这一刻）。
 */
static void unload_cb(lv_event_t *e)
{
	(void)e;
	if (M->is_open())
		M->stop_all();
}

/* ------------------------------------------------------------------ */
/* 页面构建                                                           */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_motor_row(lv_obj_t *body, int id, const char *name)
{
	static const struct {
		const char *text;
		int	    dir;
		uint32_t    color;
	} btns[] = {
		{ "正转", ECHO_MOTOR_FWD,   0x3AA0FF },
		{ "反转", ECHO_MOTOR_REV,   0xC08030 },
		{ "停止", ECHO_MOTOR_COAST, 0x3A4A60 },
	};

	lv_obj_t *row = lv_obj_create(body);

	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, ROW_H);
	lv_obj_set_style_bg_color(row, UI_C_LIST, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_pad_all(row, 6, 0);
	lv_obj_set_style_pad_column(row, 4, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *lb = lv_label_create(row);

	lv_label_set_text(lb, name);
	lv_obj_set_style_text_color(lb, UI_C_TEXT, 0);
	lv_obj_set_width(lb, 30);

	for (unsigned i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
		lv_obj_t *b = lv_button_create(row);

		lv_obj_set_size(b, BTN_W, BTN_H);
		lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].color), 0);
		lv_obj_set_style_radius(b, 6, 0);

		lv_obj_t *bl = lv_label_create(b);

		lv_label_set_text(bl, btns[i].text);
		lv_obj_center(bl);

		lv_obj_add_event_cb(b, motor_btn_cb, LV_EVENT_CLICKED,
				    (void *)(intptr_t)((id << 4) | btns[i].dir));
	}

	return row;
}

static lv_obj_t *build_motor(void)
{
	M = motor_api();	/* 拿函数表，后面所有调用都走它 */

	lv_obj_t *scr = ui_make_screen("电机测试", true);
	lv_obj_t *body = ui_screen_body(scr);

	lv_obj_set_style_pad_all(body, 6, 0);
	lv_obj_set_style_pad_row(body, 0, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	/* ---- 状态行 ---- */
	lv_obj_t *srow = lv_obj_create(body);

	lv_obj_set_width(srow, lv_pct(100));
	lv_obj_set_height(srow, STAT_H);
	lv_obj_set_style_bg_opa(srow, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(srow, 0, 0);
	lv_obj_set_style_pad_all(srow, 0, 0);
	lv_obj_remove_flag(srow, LV_OBJ_FLAG_SCROLLABLE);

	stat_lb = lv_label_create(srow);
	lv_obj_set_style_text_color(stat_lb, UI_C_DIM, 0);
	lv_label_set_long_mode(stat_lb, LV_LABEL_LONG_DOT);
	lv_obj_set_width(stat_lb, lv_pct(100));
	lv_label_set_text(stat_lb, "正在连接驱动…");
	lv_obj_center(stat_lb);

	/* ---- 两路电机 ---- */
	make_motor_row(body, ECHO_MOTOR_LEFT,  "左");
	make_motor_row(body, ECHO_MOTOR_RIGHT, "右");

	/* ---- 全部停止 ---- */
	lv_obj_t *stop = lv_button_create(body);

	lv_obj_set_width(stop, lv_pct(100));
	lv_obj_set_height(stop, 46);
	lv_obj_set_style_bg_color(stop, lv_color_hex(0xC03030), 0);
	lv_obj_set_style_radius(stop, 6, 0);
	lv_obj_add_event_cb(stop, stop_all_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *sl = lv_label_create(stop);

	lv_label_set_text(sl, "全部停止");
	lv_obj_center(sl);

	/* 离开页面强制停 */
	lv_obj_add_event_cb(scr, unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

	/* 进页面就试一次 open，失败直接把原因写在状态行上 */
	M->open();
	refresh_status();

	return scr;
}

lv_obj_t *ui_page_motortest(void)
{
	if (!page_motor)
		page_motor = build_motor();
	return page_motor;
}
