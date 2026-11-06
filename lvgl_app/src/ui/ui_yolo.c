/**
 * ui_yolo.c —— YOLO 相机页：实时预览 + 检测框 + 帧率
 *
 * 【页面布局】（320x240）
 *   ┌──────────────────────────────┐
 *   │ YOLO 相机                返回 │ 34 标题栏
 *   ├──────────────────────────────┤
 *   │        预览 320x160          │ 160  检测框叠在画面上
 *   ├──────────────────────────────┤
 *   │ 12.5 fps · 2 个目标 · 80 ms  │ 46  状态条
 *   └──────────────────────────────┘
 *
 * 【坐标系要小心：预览是"居中裁剪"，不是压缩】
 *   采集是 320x240，预览只有 320x160 —— 上下各裁掉 40 行。
 *   检测框是在【整幅 320x240】上算出来的，所以画到画面上必须 y -= 40，
 *   而且超出 0~160 的部分要丢掉（裁掉区域里的目标本来就看不见）。
 *   忘了这一步的典型症状：框整体偏下 40 像素，越靠边偏得越离谱。
 *
 * 【为什么检测框用 LVGL 对象而不是画进 canvas】
 *   canvas 每帧被整块 memcpy 覆盖，画进去的框立刻被冲掉；
 *   要保留就得在拷贝之后再画一遍，还得自己做线段裁剪。
 *   用 8 个透明的"边框对象"叠在上面更省事，位置每帧改一下就行，
 *   而且边框粗细、圆角、透明度都能直接用样式调。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ui.h"
#include "../cam/cam.h"
#include "../yolo/yolo_detect.h"

#define YOLO_MODEL	"/root/bin/model/yolov5.rknn"
#define YOLO_LABELS	"/root/bin/model/coco_80_labels_list.txt"

#define PV_W		CAM_PV_W	/* 320 */
#define PV_H		CAM_PV_H	/* 160 */
#define BAR_H		46
#define MAX_DRAW	8		/* 屏幕上最多画这么多个框 */

/* 预览是整幅图居中裁出来的，上边被裁掉多少行 —— 运行时按实际采集高度算，
 * 不写死 40（万一以后把采集改回 640x480，写死的偏移会整体错位） */
static int crop_top(void)
{
	int w = 0, h = 0;

	cam_capture_size(&w, &h);
	if (h <= PV_H)
		return 0;
	return (h - PV_H) / 2;
}

static lv_obj_t	*page_yolo;
static lv_obj_t	*canvas;
static lv_obj_t	*status_lb;
static lv_obj_t	*cpu_lb;
static lv_obj_t	*box_obj[MAX_DRAW];
static lv_obj_t	*box_lb[MAX_DRAW];
static uint16_t	*pv_buf;
static lv_timer_t *poll_timer;
static uint32_t	last_yolo_seq;
static uint32_t	last_preview_seq;
static uint64_t	cpu_last_total, cpu_last_idle;
static int	yolo_started;

/* 12 色调色板：按类别 id 取，保证同一个类永远是同一个颜色 */
static const uint32_t kPalette[12] = {
	0xFF4040, 0x40C0FF, 0x40E070, 0xFFC040, 0xC060FF, 0x40FFE0,
	0xFF70A0, 0x80A0FF, 0xA0E040, 0xE08040, 0x60E0C0, 0xD0D040,
};

static uint32_t cls_color(int cls)
{
	return kPalette[(cls % 12 + 12) % 12];
}

/* 进入页面：开相机 + 开 RGB 通路 + 起推理 */
static void yolo_page_enter(void)
{
	last_preview_seq = 0;
	cpu_last_total = cpu_last_idle = 0;
	if (!cam_running()) {
		/* /dev/video11 = rkisp mainpath，和摄像头页用的是同一个节点 */
		if (cam_start("/dev/video11", "/userdata/cam/iqfiles") != 0) {
			lv_label_set_text(status_lb, cam_last_error());
			lv_obj_set_style_text_color(status_lb,
						    lv_color_hex(0xFF6060), 0);
			return;
		}
	}

	cam_rgb_enable(1);

	if (!yolo_started) {
		if (yolo_start(YOLO_MODEL, YOLO_LABELS) == 0) {
			yolo_started = 1;
			lv_obj_set_style_text_color(status_lb, UI_C_DIM, 0);
			lv_label_set_text(status_lb, "启动中…");
		} else {
			lv_obj_set_style_text_color(status_lb,
						    lv_color_hex(0xFF6060), 0);
			lv_label_set_text(status_lb, yolo_last_error());
		}
	}
}

/* 离开页面：停推理、关 RGB 通路、停相机（省电省 CPU） */
static void yolo_page_leave(void)
{
	if (yolo_started) {
		yolo_stop();
		yolo_started = 0;
	}
	cam_rgb_enable(0);

	if (cam_running())
		cam_stop();
}

static void screen_load_cb(lv_event_t *e)
{
	(void)e;
	yolo_page_enter();
}

static void screen_unload_cb(lv_event_t *e)
{
	(void)e;
	yolo_page_leave();
}

/* ------------------------------------------------------------------ */
/* 每帧刷新                                                           */
/* ------------------------------------------------------------------ */
static void hide_all_boxes(void)
{
	for (int i = 0; i < MAX_DRAW; i++) {
		lv_obj_add_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);
	}
}

static void draw_boxes(const yolo_result *r)
{
	const int n = r->count > MAX_DRAW ? MAX_DRAW : r->count;
	const int crop = crop_top();

	for (int i = 0; i < MAX_DRAW; i++) {
		if (i >= n) {
			lv_obj_add_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}

		const yolo_box *b = &r->box[i];
		uint32_t col = cls_color(b->cls);

		/* 整幅坐标 → 预览坐标：减去上边被裁掉的部分 */
		int x = b->x;
		int y = b->y - crop;
		int w = b->w;
		int h = b->h;

		/* 完全在裁掉的区域里就不画了 */
		if (y + h <= 0 || y >= PV_H) {
			lv_obj_add_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}

		/* 贴着上下边裁一下，别让框画到 canvas 外面 */
		if (y < 0) {
			h += y;
			y = 0;
		}
		if (y + h > PV_H)
			h = PV_H - y;
		if (x < 0) {
			w += x;
			x = 0;
		}
		if (x + w > PV_W)
			w = PV_W - x;
		if (w < 2 || h < 2) {
			lv_obj_add_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}

		lv_obj_remove_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_pos(box_obj[i], x, y);
		lv_obj_set_size(box_obj[i], w, h);
		lv_obj_set_style_border_color(box_obj[i], lv_color_hex(col), 0);

		/* 标签贴在框左上角内侧，框太小就放到框下面 */
		lv_label_set_text_fmt(box_lb[i], "%s %.0f%%",
				      yolo_class_name(b->cls), b->prop * 100);
		lv_obj_set_style_text_color(box_lb[i], lv_color_hex(col), 0);
		lv_obj_set_style_bg_color(box_lb[i], lv_color_hex(0x000000), 0);
		lv_obj_remove_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);

		/* ★ 必须先强制算一次布局，否则 get_width() 返回的是上一次的旧值
		 *   （LVGL 是惰性布局，文字变了但还没算过）。旧值会让右边的标签
		 *   被 clamp 到错误位置，表现为"标签跑到左边框外面"。 */
		lv_obj_update_layout(box_lb[i]);

		int ly = y + 1;
		int lw = lv_obj_get_width(box_lb[i]);
		int lx = x + 2;

		if (ly + 16 > PV_H)
			ly = PV_H - 16;
		if (lx + lw > PV_W)
			lx = PV_W - lw;
		if (lx < 0)
			lx = 0;
		lv_obj_set_pos(box_lb[i], lx, ly);
	}
}

static void poll_cb(lv_timer_t *t)
{
	(void)t;
	if (lv_screen_active() != page_yolo)
		return;

	/* ---- 1. 预览 ---- */
	uint32_t s = cam_preview(pv_buf, PV_W * PV_H * 2);

	if (s && s != last_preview_seq) {
		last_preview_seq = s;
		lv_obj_invalidate(canvas);
	} else if (!cam_running()) {
		/* 相机没跑起来时把错误写在状态条上，别让人对着黑屏猜 */
		lv_label_set_text(status_lb, cam_last_error());
	}

	/* ---- 2. 检测结果（seq 变了才重画）---- */
	yolo_result r;

	if (yolo_get(&r) != 0 && r.seq != last_yolo_seq) {
		last_yolo_seq = r.seq;
		draw_boxes(&r);
	}

	/* ---- 3. 状态文字 ---- */
	const char *st = yolo_status();

	if (st && st[0] && strcmp(lv_label_get_text(status_lb), st) != 0)
		lv_label_set_text(status_lb, st);
}

/* /proc/stat 的相邻两次累计值之差，显示整机 CPU 占用。 */
static void cpu_cb(lv_timer_t *t)
{
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	uint64_t total, idle_total;
	char line[256];
	FILE *fp;
	(void)t;
	if (lv_screen_active() != page_yolo) {
		cpu_last_total = cpu_last_idle = 0;
		return;
	}
	fp = fopen("/proc/stat", "r");
	if (!fp)
		return;
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}
	fclose(fp);
	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &system, &idle, &iowait,
		   &irq, &softirq, &steal) != 8)
		return;
	total = user + nice + system + idle + iowait + irq + softirq + steal;
	idle_total = idle + iowait;
	if (cpu_last_total && total > cpu_last_total) {
		uint64_t dt = total - cpu_last_total;
		uint64_t di = idle_total - cpu_last_idle;
		int cpu = (int)((100 * (dt - (di > dt ? dt : di)) + dt / 2) / dt);
		lv_label_set_text_fmt(cpu_lb, "CPU %d%%", cpu);
		if (getenv("YOLO_STATS_LOG"))
			printf("[yolo-stats] CPU %d%% %s\n", cpu, yolo_status());
	}
	cpu_last_total = total;
	cpu_last_idle = idle_total;
}

/* ------------------------------------------------------------------ */
/* 页面构建                                                           */
/* ------------------------------------------------------------------ */
static lv_obj_t *build_yolo(void)
{
	lv_obj_t *scr = ui_make_screen("YOLO 相机", true);
	lv_obj_t *body = ui_screen_body(scr);

	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_style_pad_row(body, 0, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	pv_buf = (uint16_t *)lv_malloc(PV_W * PV_H * 2);
	if (!pv_buf) {
		lv_label_set_text(lv_label_create(body), "内存不足");
		return scr;
	}
	memset(pv_buf, 0, PV_W * PV_H * 2);

	canvas = lv_canvas_create(body);
	lv_canvas_set_buffer(canvas, pv_buf, PV_W, PV_H, LV_COLOR_FORMAT_RGB565);
	lv_obj_set_size(canvas, PV_W, PV_H);
	lv_obj_set_style_border_width(canvas, 0, 0);
	lv_obj_set_style_pad_all(canvas, 0, 0);
	cpu_lb = lv_label_create(canvas);
	lv_label_set_text(cpu_lb, "CPU --");
	lv_obj_set_style_text_font(cpu_lb, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(cpu_lb, lv_color_hex(0xFFFFFF), 0);
	lv_obj_set_style_bg_color(cpu_lb, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(cpu_lb, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(cpu_lb, 2, 0);
	lv_obj_align(cpu_lb, LV_ALIGN_TOP_RIGHT, -3, 3);

	/* ---- 检测框（预建 8 个，靠显示/隐藏切换）---- */
	for (int i = 0; i < MAX_DRAW; i++) {
		box_obj[i] = lv_obj_create(canvas);
		lv_obj_set_size(box_obj[i], 10, 10);
		lv_obj_set_style_bg_opa(box_obj[i], LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(box_obj[i], 2, 0);
		lv_obj_set_style_border_color(box_obj[i],
					      lv_color_hex(0xFF4040), 0);
		lv_obj_set_style_radius(box_obj[i], 0, 0);
		lv_obj_set_style_pad_all(box_obj[i], 0, 0);
		lv_obj_remove_flag(box_obj[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(box_obj[i], LV_OBJ_FLAG_HIDDEN);

		/* 标签是 canvas 的子对象（不是框的子对象）：
		 * 框的子对象会跟着框裁剪，框一小标签就看不见了 */
		box_lb[i] = lv_label_create(canvas);
		lv_obj_set_style_text_font(box_lb[i], &lv_font_montserrat_14, 0);
		lv_obj_set_style_bg_opa(box_lb[i], LV_OPA_50, 0);
		lv_obj_set_style_pad_all(box_lb[i], 1, 0);
		lv_label_set_text(box_lb[i], "");
		lv_obj_add_flag(box_lb[i], LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_move_foreground(cpu_lb);

	/* ---- 状态条 ---- */
	lv_obj_t *row = lv_obj_create(body);

	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, BAR_H);
	lv_obj_set_style_bg_color(row, UI_C_BAR, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_pad_all(row, 6, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	status_lb = lv_label_create(row);
	lv_obj_set_width(status_lb, lv_pct(100));
	lv_obj_set_style_text_color(status_lb, UI_C_DIM, 0);
	lv_label_set_long_mode(status_lb, LV_LABEL_LONG_DOT);
	lv_label_set_text(status_lb, "准备中…");

	/* ---- 生命周期 + 轮询 ---- */
	lv_obj_add_event_cb(scr, screen_load_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(scr, screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START,
			    NULL);

	last_yolo_seq = 0;
	poll_timer = lv_timer_create(poll_cb, 100, NULL);
	lv_timer_create(cpu_cb, 1000, NULL);

	return scr;
}

lv_obj_t *ui_page_yolo(void)
{
	if (!page_yolo)
		page_yolo = build_yolo();
	return page_yolo;
}
