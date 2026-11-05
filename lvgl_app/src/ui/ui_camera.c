/**
 * ui_camera.c —— 摄像头页面：实时预览 + 拍照 + 录像
 *
 * 【页面布局】（屏幕 320x240）
 *   ┌──────────────────────────────┐
 *   │ 摄像头                    <  │ 34px 标题栏（ui_make_screen 自带）
 *   ├──────────────────────────────┤
 *   │                              │
 *   │       预览 canvas            │ 160px
 *   │       (320x160)              │
 *   │                              │
 *   ├──────────────────────────────┤
 *   │ [拍照]   [录像]   状态/计数   │ 46px
 *   └──────────────────────────────┘
 *
 * 【为什么预览用 canvas 而不是 image】
 *   canvas 直接绑定一块 RGB565 内存，采集线程把像素写进去、LVGL 每帧照原样贴到屏上。
 *   而 image 需要 image_dsc 管理生命周期（描述符要一直有效、不能是栈变量），
 *   在"每 40ms 换一次内容"的场景下用 canvas 更直接、也少一次数据拷贝。
 *
 * 【为什么用 lv_timer 轮询而不是回调通知】
 *   采集线程是纯 C 的、不知道 LVGL 存在。要让它主动通知 UI 就得跨线程调
 *   lv_obj_invalidate()，那是 LVGL 的单线程大忌（会随机花屏/崩溃）。
 *   所以反过来：UI 用 lv_timer 主动去问"有新帧吗"，用 seq 号判断。
 *   这跟 WiFi 扫描页"用 lv_timer 查 seq 变化再重建列表"是同一套做法。
 *
 * 【页面进出时启停摄像头】
 *   进入页面才 cam_start()，离开就 cam_stop()。
 *   理由：摄像头满速采集时的 YUV→RGB 转换会吃掉这颗单核 A7 相当一部分算力，
 *   一直开着会让其它页面（尤其语音对话的音频线程）变卡。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ui.h"
#include "../cam/cam.h"

/* 存照片/录像的目录。放 /tmp 是因为 rootfs 只剩 3MB，
 * 而 /tmp 是 27MB 的 tmpfs —— 代价是重启就没了，正好符合"临时抓拍"。 */
#define CAM_DIR		"/tmp/cam"

/*
 * ★ 预览区高度【不要在这里自己定义】—— 直接用 cam.h 的 CAM_PV_H。
 *
 *   曾经这里写死了 PV_H=160，而 cam.h 里 CAM_PV_H=240，两个宏各自独立。
 *   结果 cam_preview 按 240 行拷数据，canvas 缓冲只有 160 行 →
 *   每帧越界写 50KB，踩坏 LVGL 对象内存：
 *   画面下半部分异常 + 跑十几秒后进程静默消失（无任何报错）。
 *
 *   "同一个概念只允许有一个定义" —— pv_buf 的大小和
 *   cam_preview 拷贝的大小必须来自同一个宏，否则迟早错位。
 */
#define PV_H		CAM_PV_H
#define BTN_H		46

static lv_obj_t	*page_camera;
static lv_obj_t	*canvas;
static lv_obj_t	*btn_photo_lb;		/* 拍照按钮上的字（要能改，做反馈）*/
static lv_obj_t	*btn_rec_lb;		/* AVI 录像按钮上的字 */
static lv_obj_t	*btn_h265_lb;		/* H.265/MP4 录像按钮上的字 */
static lv_obj_t	*status_lb;
static lv_obj_t	*stats_lb;		/* 预览帧率 / 整机 CPU 占用 */
static lv_obj_t	*rec_dot;		/* 录像中闪的红点 */

static uint16_t	*pv_buf;		/* canvas 的像素缓冲（RGB565）*/
static uint32_t	 last_seq;
static int	 shot_idx;		/* 第几张照片，用来生成文件名 */
static int	 video_idx;
static lv_timer_t *poll_timer;
static int	 cam_on;		/* 我们这边记录的"摄像头是否已启动" */

/* /proc/stat 是累计值，CPU 占用率必须用相邻两次采样的差值计算。 */
static uint64_t	 cpu_last_total;
static uint64_t	 cpu_last_idle;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */
static void ensure_dir(void)
{
	struct stat st;

	if (stat(CAM_DIR, &st) == 0)
		return;
	mkdir(CAM_DIR, 0755);
}

/* 只保留最近 N 个文件，防止 /tmp 被写满。
 * 用最简单的做法：按序号覆盖 —— 超过 N 张就回到第 0 张重新写。 */
#define KEEP_SHOTS	20
#define KEEP_VIDEOS	5

static int next_index(int *idx, int keep)
{
	int v = *idx;

	*idx = (*idx + 1) % keep;
	return v;
}

/* 读取整机 CPU 的累计时间。idle 把 iowait 也算入空闲时间。 */
static int read_cpu_ticks(uint64_t *total, uint64_t *idle)
{
	unsigned long long user, nice, system, idle_tick;
	unsigned long long iowait, irq, softirq, steal;
	char line[256];
	FILE *fp = fopen("/proc/stat", "r");

	if (!fp)
		return -1;
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &system, &idle_tick, &iowait,
		   &irq, &softirq, &steal) != 8)
		return -1;

	*idle = (uint64_t)idle_tick + (uint64_t)iowait;
	*total = (uint64_t)user + (uint64_t)nice + (uint64_t)system +
		 (uint64_t)idle_tick + (uint64_t)iowait + (uint64_t)irq +
		 (uint64_t)softirq + (uint64_t)steal;
	return 0;
}

/* 每秒更新一次，避免统计标签本身频繁触发重绘。 */
static void stats_cb(lv_timer_t *t)
{
	static uint32_t prev_seq;
	static uint32_t prev_ms;
	uint64_t total, idle;
	uint32_t seq, now, elapsed, fps10 = 0;
	int cpu = -1;
	char text[40];

	(void)t;

	if (lv_screen_active() != page_camera) {
		prev_seq = 0;
		prev_ms = 0;
		cpu_last_total = 0;
		cpu_last_idle = 0;
		return;
	}

	/*
	 * last_seq 来自采集线程的帧序号。即使 UI 只每 50ms 取一次图，
	 * 序号仍会按摄像头实际完成的每一帧递增，因此不会被 20Hz 的
	 * 页面轮询频率封顶。
	 */
	now = lv_tick_get();
	seq = last_seq;
	if (cam_on && prev_ms && seq >= prev_seq) {
		elapsed = now - prev_ms;
		if (elapsed)
			fps10 = (uint32_t)(((uint64_t)(seq - prev_seq) * 10000 +
					 elapsed / 2) / elapsed);
	}
	prev_seq = seq;
	prev_ms = now;

	if (read_cpu_ticks(&total, &idle) == 0) {
		if (cpu_last_total && total > cpu_last_total) {
			uint64_t total_delta = total - cpu_last_total;
			uint64_t idle_delta = idle - cpu_last_idle;

			if (idle_delta > total_delta)
				idle_delta = total_delta;
			cpu = (int)((100 * (total_delta - idle_delta) +
				     total_delta / 2) / total_delta);
		}
		cpu_last_total = total;
		cpu_last_idle = idle;
	}

	if (cpu >= 0)
		snprintf(text, sizeof(text), "CAM %u.%u FPS  CPU %d%%",
			 (unsigned)(fps10 / 10), (unsigned)(fps10 % 10), cpu);
	else
		snprintf(text, sizeof(text), "CAM %u.%u FPS  CPU --",
			 (unsigned)(fps10 / 10), (unsigned)(fps10 % 10));
	lv_label_set_text(stats_lb, text);

	/* 测试时显式打开，正常运行不刷日志。 */
	if (getenv("CAM_STATS_LOG"))
		printf("[camera-stats] %s\n", text);
}

/* ------------------------------------------------------------------ */
/* 拍照 / 录像按钮                                                    */
/* ------------------------------------------------------------------ */
static void photo_cb(lv_event_t *e)
{
	char path[96];
	int n;

	(void)e;

	if (!cam_on) {
		lv_label_set_text(status_lb, "摄像头未就绪");
		return;
	}

	n = next_index(&shot_idx, KEEP_SHOTS);
	snprintf(path, sizeof(path), CAM_DIR "/shot_%02d.bmp", n);

	if (cam_photo(path) == 0) {
		lv_label_set_text(btn_photo_lb, "已拍");
		lv_label_set_text(status_lb, path);
	} else {
		lv_label_set_text(status_lb, "上一张还在写");
	}
}

static int stop_recording(void)
{
	int rc = cam_record_stop();

	lv_label_set_text(btn_rec_lb, "AVI");
	lv_label_set_text(btn_h265_lb, "H265");
	lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
	if (rc == 0)
		lv_label_set_text(status_lb, "录像已保存到 " CAM_DIR);
	else
		lv_label_set_text(status_lb, cam_last_error());
	return rc;
}

static void rec_cb(lv_event_t *e)
{
	char path[96];
	int n;

	(void)e;

	if (!cam_on) {
		lv_label_set_text(status_lb, "摄像头未就绪");
		return;
	}

	if (cam_recording()) {
		stop_recording();
		return;
	}

	n = next_index(&video_idx, KEEP_VIDEOS);
	snprintf(path, sizeof(path), CAM_DIR "/video_%02d.avi", n);

	if (cam_record_start(path, 8) == 0) {
		lv_label_set_text(btn_rec_lb, "停止");
		lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(status_lb, path);
	} else {
		lv_label_set_text(status_lb, cam_last_error());
	}
}

static void h265_cb(lv_event_t *e)
{
	char path[96];
	int n;

	(void)e;
	if (!cam_on) {
		lv_label_set_text(status_lb, "摄像头未就绪");
		return;
	}
	if (cam_recording()) {
		stop_recording();
		return;
	}

	n = next_index(&video_idx, KEEP_VIDEOS);
	snprintf(path, sizeof(path), CAM_DIR "/video_%02d_h265.mp4", n);
	if (cam_record_start_h265(path) == 0) {
		lv_label_set_text(btn_h265_lb, "停止");
		lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(status_lb, path);
	} else {
		lv_label_set_text(status_lb, cam_last_error());
	}
}

/* ------------------------------------------------------------------ */
/* 预览刷新（lv_timer，主线程）                                        */
/* ------------------------------------------------------------------ */
/* 上一次显示的状态文字，用于避免无意义的 lv_label_set_text */
static char last_status[80];

/*
 * 摄像头启动失败后的重试间隔（毫秒）。
 *
 * 为什么不干脆放弃：驱动可能是后加载的（手动 insmod、或开机脚本慢一步），
 * 等一会儿它就有了。但也不能每 20ms 试一次 —— 见 poll_cb 里的说明。
 * 2 秒是权衡：人感觉不到明显延迟，日志和内存也不会失控。
 */
#define RETRY_MS	2000

static void poll_cb(lv_timer_t *t)
{
	static int h265_autotest_state;
	uint32_t seq;
	char buf[80];

	(void)t;

	/*
	 * ★ 页面是否可见，用 lv_screen_active() 判断而不是靠事件标志。
	 *   LVGL 切页时旧 screen 还在内存里，只是不参与渲染 ——
	 *   它的 lv_timer 照样会跑。不判断的话，离开页面后摄像头还在跑，
	 *   白白吃 CPU，还会和语音线程抢。
	 */
	if (lv_screen_active() != page_camera) {
		if (cam_on) {
			cam_stop();
			cam_on = 0;
			lv_label_set_text(status_lb, "已暂停");
		}
		return;
	}

	if (!cam_on) {
		/*
		 * ★★★ 重试必须限速，不能每个 tick 都试。
		 *
		 *   poll_cb 是 20ms 一次。原来这里无条件调 cam_start()，
		 *   于是"打不开就一直每秒试 50 次"。这个频率本身就有三个害处：
		 *
		 *   ① 日志刷屏：实测一次误操作刷了 169 行 "打开失败"，
		 *      把真正有用的信息全冲掉了
		 *   ② 内存被吃光：cam_start 失败路径原本漏 200KB（已在 cam.c
		 *      修掉），50 次/秒 = 10MB/s，几十秒就把 54MB 的板子撑爆
		 *      → 内核 OOM 杀掉 lvgl_app（实测 anon-rss 35MB）
		 *   ③ 错误原因不会因为重试变快而消失 —— 模块没加载这种事，
		 *      20ms 试一万次也没用，2 秒试一次同样能等到它加载上来
		 *
		 *   所以：失败后至少隔 RETRY_MS 再试。2 秒足够等到别人
		 *   insmod 完驱动，也不会让日志和内存失控。
		 */
		static uint32_t next_try_ms;
		uint32_t now = lv_tick_get();

		if (now < next_try_ms)
			return;

		ensure_dir();
		if (cam_start("/dev/video11", "/userdata/cam/iqfiles") != 0) {
			/*
			 * 给用户看得懂的话，而不是直接抛 errno 字符串。
			 * "No such file or directory" 对使用者毫无意义，
			 * 真正的原因是设备节点不存在 = 摄像头驱动没装。
			 */
			if (access("/dev/video11", F_OK) != 0)
				lv_label_set_text(status_lb,
						  "摄像头未就绪（驱动未加载）");
			else
				lv_label_set_text(status_lb, cam_last_error());

			next_try_ms = now + RETRY_MS;
			return;
		}

		cam_on = 1;
		last_seq = 0;		/* cam_start 会把采集序号从 0 重新计数 */
		last_status[0] = 0;	/* 状态变了，强制刷新文字 */
		lv_label_set_text(status_lb, cam_status());
		return;		/* 这一轮先不画，等下一帧 */
	}

	/* 板端回归测试开关：真实相机就绪后录 2 秒 H.265/MP4。默认完全
	 * 不运行；部署验证时由守护进程环境临时打开。 */
	if (getenv("CAM_H265_AUTOTEST")) {
		if (h265_autotest_state == 0) {
			h265_cb(NULL);
			h265_autotest_state = cam_recording() ? 1 : 2;
		} else if (h265_autotest_state == 1 && cam_record_seconds() >= 2) {
			stop_recording();
			h265_autotest_state = 2;
		}
	}

	seq = cam_preview(pv_buf, CAM_PV_W * PV_H * 2);
	if (seq == 0 || seq == last_seq)
		return;		/* 没新帧就不重绘，省 SPI 带宽 */
	last_seq = seq;

	/* 像素已经写进 pv_buf，告诉 LVGL 这块区域要重画 */
	lv_obj_invalidate(canvas);

	/* 状态栏：录像中优先显示秒数，否则显示拍照张数 */
	if (cam_recording()) {
		snprintf(buf, sizeof(buf), "REC %ds  %s",
			 cam_record_seconds(), cam_last_photo());
		/* 红点闪烁：靠秒数奇偶，不需要额外定时器 */
		if (cam_record_seconds() % 2)
			lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
	} else {
		/* 达到 10 秒上限自动停止时，也把两个按钮恢复。 */
		lv_label_set_text(btn_rec_lb, "AVI");
		lv_label_set_text(btn_h265_lb, "H265");
		lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
		snprintf(buf, sizeof(buf), "%s  拍照 %u 张",
			 cam_status(), (unsigned)cam_photo_count());
	}
	/*
	 * ★★★ 只在文字真的变了才设置。
	 *
	 *   lv_label_set_text 不管内容是否相同，都会把整个 label 区域
	 *   标脏 → 触发重绘 → 写 fb0 → 驱动再推一次 SPI。
	 *
	 *   而这一行每秒被调 20 次，内容却是"320x240 3A on 拍照 0 张"
	 *   这种几十秒都不变的东西 —— 等于每秒白白多刷 20 次底部状态栏，
	 *   还顺带把 LVGL 的脏区从"只有 canvas"扩大到"整屏"。
	 *
	 *   实测：加这个判断前，每帧脏页覆盖 0~239 全屏；
	 *   这正是"上半部分闪"的元凶之一：本来只有画面在变，
	 *   结果状态栏也跟着刷，SPI 被切成更多段。
	 */
	if (strcmp(buf, last_status) != 0) {
		snprintf(last_status, sizeof(last_status), "%s", buf);
		lv_label_set_text(status_lb, buf);
	}
}

/* ------------------------------------------------------------------ */
/* 页面构建                                                           */
/* ------------------------------------------------------------------ */
static lv_obj_t *build_camera(void)
{
	lv_obj_t *scr = ui_make_screen("摄像头", true);
	lv_obj_t *body = ui_screen_body(scr);
	lv_obj_t *row;

	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_style_pad_row(body, 0, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	/* ---- 预览 canvas ---- */
	pv_buf = malloc(CAM_PV_W * PV_H * 2);
	if (!pv_buf) {
		lv_label_set_text(lv_label_create(body), "内存不足");
		return scr;
	}
	memset(pv_buf, 0, CAM_PV_W * PV_H * 2);

	canvas = lv_canvas_create(body);
	lv_canvas_set_buffer(canvas, pv_buf, CAM_PV_W, PV_H, LV_COLOR_FORMAT_RGB565);
	lv_obj_set_size(canvas, CAM_PV_W, PV_H);
	lv_obj_set_style_border_width(canvas, 0, 0);
	lv_obj_set_style_pad_all(canvas, 0, 0);

	/* 叠在画面右上角，半透明底色保证明暗场景下都看得清。 */
	stats_lb = lv_label_create(canvas);
	lv_label_set_text(stats_lb, "CAM 0.0 FPS  CPU --");
	lv_obj_set_style_text_color(stats_lb, lv_color_white(), 0);
	lv_obj_set_style_bg_color(stats_lb, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(stats_lb, LV_OPA_60, 0);
	lv_obj_set_style_pad_hor(stats_lb, 4, 0);
	lv_obj_set_style_pad_ver(stats_lb, 2, 0);
	lv_obj_align(stats_lb, LV_ALIGN_TOP_RIGHT, -4, 4);

	/* ---- 按钮行 ---- */
	row = lv_obj_create(body);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, BTN_H);
	lv_obj_set_style_bg_color(row, UI_C_BAR, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_pad_all(row, 4, 0);
	lv_obj_set_style_pad_column(row, 6, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* 拍照 */
	lv_obj_t *b1 = lv_button_create(row);
	lv_obj_set_size(b1, 64, 34);
	lv_obj_set_style_bg_color(b1, UI_C_ACCENT, 0);
	btn_photo_lb = lv_label_create(b1);
	lv_label_set_text(btn_photo_lb, "拍照");
	lv_obj_center(btn_photo_lb);
	lv_obj_add_event_cb(b1, photo_cb, LV_EVENT_CLICKED, NULL);

	/* 原始 AVI（兼容旧格式） */
	lv_obj_t *b2 = lv_button_create(row);
	lv_obj_set_size(b2, 64, 34);
	lv_obj_set_style_bg_color(b2, lv_color_hex(0xC03030), 0);
	btn_rec_lb = lv_label_create(b2);
	lv_label_set_text(btn_rec_lb, "AVI");
	lv_obj_center(btn_rec_lb);
	lv_obj_add_event_cb(b2, rec_cb, LV_EVENT_CLICKED, NULL);

	/* H.265 硬编并封装 MP4 */
	lv_obj_t *b3 = lv_button_create(row);
	lv_obj_set_size(b3, 64, 34);
	lv_obj_set_style_bg_color(b3, lv_color_hex(0x8050C0), 0);
	btn_h265_lb = lv_label_create(b3);
	lv_label_set_text(btn_h265_lb, "H265");
	lv_obj_center(btn_h265_lb);
	lv_obj_add_event_cb(b3, h265_cb, LV_EVENT_CLICKED, NULL);

	/* 录像中的红点（默认隐藏）*/
	rec_dot = lv_obj_create(row);
	lv_obj_set_size(rec_dot, 10, 10);
	lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(rec_dot, lv_color_hex(0xFF3030), 0);
	lv_obj_set_style_border_width(rec_dot, 0, 0);
	lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);

	/* 状态文字（吃掉剩下的宽度）*/
	status_lb = lv_label_create(row);
	lv_obj_set_flex_grow(status_lb, 1);
	lv_obj_set_style_text_color(status_lb, UI_C_DIM, 0);
	lv_label_set_long_mode(status_lb, LV_LABEL_LONG_DOT);
	lv_label_set_text(status_lb, "准备中");

	/* ---- 轮询定时器 ---- */
	last_seq = 0;
	poll_timer = lv_timer_create(poll_cb, 50, NULL);
	lv_timer_create(stats_cb, 1000, NULL);

	return scr;
}

lv_obj_t *ui_page_camera(void)
{
	if (!page_camera)
		page_camera = build_camera();
	return page_camera;
}

/* 自测/调试用：拿到当前是否在录像，验证按钮确实改变了状态 */
bool ui_camera_recording(void)
{
	return cam_recording();
}

/**
 * 调试用：按名字打开摄像头页。
 * 板子上用手指点准一行很难，所以留个后门（和 LVGL_START_PAGE 同一思路）。
 * 用法：直接调 ui_goto("camera")。
 */
int ui_camera_goto(void)
{
	ui_nav_push(ui_page_camera());
	return 0;
}
