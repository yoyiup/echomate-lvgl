/**
 * ui_netcheck.c —— 网络自检页
 *
 * 五层逐项打勾，卡在哪一层一眼看到。
 * 这是把上一轮那个"显示已连接但完全不通"的坑
 * 变成可视化诊断工具的产物。
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "../net/netcheck.h"

static lv_obj_t *page_nc;
static lv_obj_t *nc_row_val[NETCHECK_STEPS];	/* 每行的"结果"标签 */
static lv_obj_t *nc_row_dot[NETCHECK_STEPS];	/* 每行左侧状态点 */
static lv_obj_t *nc_summary;
static int nc_last_seq = -1;

/* 状态点颜色：绿=通过 红=失败 灰=未测 */
static lv_color_t dot_color(int ok)
{
	if (ok == 1)
		return lv_color_hex(0x5FD08A);
	if (ok == 0)
		return lv_color_hex(0xD97070);
	return lv_color_hex(0x4A5A7A);
}

static void nc_render(const struct netcheck_result *r)
{
	int i;

	for (i = 0; i < NETCHECK_STEPS; i++) {
		const struct netcheck_step *s = &r->steps[i];

		if (nc_row_dot[i])
			lv_obj_set_style_bg_color(nc_row_dot[i], dot_color(s->ok), 0);

		if (nc_row_val[i]) {
			if (s->ok == -1) {
				lv_label_set_text(nc_row_val[i], "待测");
				lv_obj_set_style_text_color(nc_row_val[i], UI_C_DIM, 0);
			} else if (s->ok) {
				lv_label_set_text_fmt(nc_row_val[i], "通过 %s (%dms)",
						      s->detail, s->ms);
				lv_obj_set_style_text_color(nc_row_val[i],
							    lv_color_hex(0x5FD08A), 0);
			} else {
				lv_label_set_text_fmt(nc_row_val[i], "失败 %s",
						      s->detail);
				lv_obj_set_style_text_color(nc_row_val[i],
							    lv_color_hex(0xD97070), 0);
			}
		}
	}

	if (nc_summary) {
		lv_label_set_text(nc_summary, r->summary[0] ? r->summary :
				  (r->state == NETCHECK_RUNNING ? "检测中..." : ""));
	}
}

static void nc_poll_cb(lv_timer_t *t)
{
	struct netcheck_result r;
	int seq;

	(void)t;

	if (lv_screen_active() != page_nc)
		return;

	seq = netcheck_get(&r);
	if (seq == nc_last_seq)
		return;
	nc_last_seq = seq;
	nc_render(&r);
}

static void nc_rerun_cb(lv_event_t *e)
{
	(void)e;
	netcheck_start();
	ui_status("正在检测网络...");
}

static lv_obj_t *build_netcheck(void)
{
	lv_obj_t *scr = ui_make_screen("网络自检", true);
	lv_obj_t *body = ui_screen_body(scr);
	int i;

	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	/* ---- 六个检查项 ----
	 * 行高定 30 而不是 34：6×30=180 刚好塞进可视区（约 178~184），
	 * 加一条摘要行也不用滚。16px 的字在 30px 行里上下各留 7px，
	 * 视觉上比 34 更紧凑但没挤。
	 * （原来是 5 项 × 34 = 170，加第 6 项就溢出了）*/
	for (i = 0; i < NETCHECK_STEPS; i++) {
		lv_obj_t *row = lv_obj_create(body);

		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, 30);
		lv_obj_set_style_bg_color(row, UI_C_LIST, 0);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(row, 10, 0);
		lv_obj_set_style_pad_ver(row, 0, 0);
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, UI_C_LINE, 0);
		lv_obj_set_style_radius(row, 0, 0);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 8, 0);

		/* 左侧状态点：用一个 10x10 的圆角方块当指示灯。
		 * 用颜色而不是 ✓ ✗ 字符 —— 字模里没有勾叉字形
		 * （它们也是 Unicode 符号区），会渲染成空白。 */
		lv_obj_t *dot = lv_obj_create(row);

		lv_obj_set_size(dot, 10, 10);
		lv_obj_set_style_radius(dot, 5, 0);
		lv_obj_set_style_bg_color(dot, dot_color(-1), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(dot, 0, 0);
		lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
		nc_row_dot[i] = dot;

		/* 层次名 */
		lv_obj_t *nm = lv_label_create(row);
		static const char *names[NETCHECK_STEPS] = {
			"关联", "IP地址", "网关", "DNS", "外网", "时间"
		};

		lv_label_set_text(nm, names[i]);
		lv_obj_set_style_text_color(nm, UI_C_TEXT, 0);
		lv_obj_set_width(nm, 56);

		/* 结果 */
		nc_row_val[i] = lv_label_create(row);
		lv_label_set_text(nc_row_val[i], "待测");
		lv_obj_set_style_text_color(nc_row_val[i], UI_C_DIM, 0);
		/* 限宽 + 省略号：detail 里可能有长 IP 或域名 */
		lv_obj_set_flex_grow(nc_row_val[i], 1);
		lv_label_set_long_mode(nc_row_val[i], LV_LABEL_LONG_DOT);
	}

	/* ---- 结论 ---- */
	nc_summary = lv_label_create(body);
	lv_label_set_text(nc_summary, "");
	lv_obj_set_style_text_color(nc_summary, UI_C_ACCENT, 0);
	lv_obj_set_style_pad_all(nc_summary, 8, 0);

	/* ---- 重新检测按钮 ---- */
	lv_obj_t *btn = lv_button_create(body);

	lv_obj_set_size(btn, 120, 30);
	lv_obj_set_style_bg_color(btn, UI_C_ACCENT, 0);
	lv_obj_set_style_radius(btn, 6, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_add_event_cb(btn, nc_rerun_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *bl = lv_label_create(btn);

	lv_label_set_text(bl, "重新检测");
	lv_obj_set_style_text_color(bl, lv_color_hex(0x0A1628), 0);
	lv_obj_center(bl);

	/* 轮询结果 */
	lv_timer_create(nc_poll_cb, 150, NULL);

	/* 进页面就自动跑一次 */
	netcheck_start();

	return scr;
}

lv_obj_t *ui_page_netcheck(void)
{
	if (!page_nc)
		page_nc = build_netcheck();
	return page_nc;
}
