/**
 * main.c —— LVGL 9 应用入口（D8：多页面版本）
 *
 * 这个文件现在只做四件事，界面代码全在 src/ui/ 下：
 *   1. lv_init()
 *   2. 把 /dev/fb0 接成 LVGL 的显示后端
 *   3. 找到触摸的 event 节点接成输入后端
 *   4. 挂上主页，进主循环
 *
 * 【LVGL 9 相对 8 最大的差别】
 * 9 自带 Linux 的 fbdev 和 evdev 后端，不用自己写 flush_cb、
 * 不用自己解析 struct input_event。两行就接通了。
 *
 * 【为什么接得这么容易】
 * 因为驱动已经把硬件"变成文件"了：
 *   lcd.ko   → /dev/fb0          (mmap 一块内存就是显存)
 *   touch.ko → /dev/input/eventN (read 出来就是一串 input_event)
 * LVGL 只认这两个路径，它不知道驱动是谁写的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/file.h>		/* flock：单实例保护 */
#include <linux/input.h>

#include "lvgl/lvgl.h"
#include "ui/ui.h"
#include "wifi/wifi_scan.h"
#include "wifi/wifi_conn.h"
#include "net/netcheck.h"
#include "net/weather.h"
#include "net/timesync.h"
#include "lv_wake_bridge.h"

#define W 320
#define H 240

/* ---------- 时基 ----------
 * LVGL 不知道真实时间过得多快，要手动喂 tick。
 * lv_tick_inc 喂的是"距上次过了多少毫秒"。
 */
static uint32_t last_ms;

static void tick_poll(void)
{
	struct timespec ts;
	uint32_t now, delta;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	delta = now - last_ms;
	if (delta) {
		lv_tick_inc(delta);
		last_ms = now;
	}
}

/* ---------- 找触摸设备 ----------
 * 判断依据是"这个设备有没有 ABS 轴"，而不是"它叫什么名字"。
 * 这样换成自己写的驱动（echo_my_touch）也照样能找到，不用改代码。
 *
 * ARM32 上 unsigned long 是 32 位，ABS_MT_POSITION_X = 53，
 * 所以位图要按"第几个 long"索引：53 / 32 = 1，第 1 个 long 的第 21 位。
 */
#define ABS_BIT_IS_SET(bits, code) \
	((bits)[(code) / (8 * sizeof(unsigned long))] & \
	 (1UL << ((code) % (8 * sizeof(unsigned long)))))

static int has_abs_axis(const char *path)
{
	unsigned long absbits[2] = { 0 };
	int fd, ok = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;

	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
		if (ABS_BIT_IS_SET(absbits, ABS_MT_POSITION_X) ||
		    ABS_BIT_IS_SET(absbits, ABS_X))
			ok = 1;
	}
	close(fd);
	return ok;
}

int main(void)
{
	lv_display_t *disp;
	lv_obj_t *home;
	char touch_path[32] = "";
	const char *env;
	long run_sec;
	int i;
	time_t start;
	time_t last_time_check = 0;

	/* 行缓冲：输出重定向到文件时也能及时看到，不被块缓冲吞掉 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/*
	 * ---------- 单实例保护 ----------
	 *
	 * 【为什么需要】真实踩过：调试时手动 setsid 起了一个，守护又拉起一个，
	 *   两个 lvgl_app 同时打开 /dev/fb0、各自往同一块显存里画 ——
	 *   屏幕就在两个页面的画面之间来回跳（表现为"轮次在 0 和 2 之间闪"），
	 *   看起来像 UI 在乱刷，其实是有两个进程在轮流覆盖同一块 framebuffer。
	 *
	 * 【为什么用 flock 而不是查 pidof】
	 *   pidof 有窗口期：刚启动还没改进程名、或者两个同时启动都会漏判。
	 *   flock 是内核保证的，进程一退出锁自动释放，守护重启也不会被误拦。
	 *
	 * 第二次启动的实例直接退出，返回码 0 —— 对守护来说"已经有一个在跑"
	 * 不是错误，不用反复重试。
	 */
	static int lock_fd = -1;

	lock_fd = open("/var/run/lvgl_app.lock", O_RDWR | O_CREAT, 0644);
	if (lock_fd >= 0 &&
	    flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "[启动] 已有 lvgl_app 在运行，本次退出（避免抢 /dev/fb0）\n");
		return 0;
	}
	/* 锁 fd 故意不关：一关锁就释放了。进程退出时内核自动释放。 */

	printf("========================================\n");
	printf("  回声伙伴 Echo-Mate  LVGL %d.%d.%d\n",
	       lv_version_major(), lv_version_minor(), lv_version_patch());
	printf("========================================\n\n");

	/* ---------- 0. 时区 ----------
	 * 必须在任何 localtime()/strftime() 之前。
	 *
	 * 板子上没有 /usr/share/zoneinfo，所以 TZ=Asia/Shanghai 是【无效】的
	 * （实测会静默退回 UTC），只能用 POSIX TZ 字符串 "CST-8"。
	 * 注意 POSIX 的符号和直觉相反：CST-8 表示 本地 = UTC + 8。
	 *
	 * 内核里的时间始终是 UTC（这是对的，别去改内核时间），
	 * 只有显示层做 +8 偏移。 */
	timesync_setup_tz();

	/* ---------- 1. LVGL 内核 ---------- */
	lv_init();
	printf("[1] lv_init() 完成\n");

	/* ---------- 2. 显示：fbdev ---------- */
	disp = lv_linux_fbdev_create();
	if (!disp) {
		printf("错误: lv_linux_fbdev_create() 失败\n");
		return 1;
	}
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	printf("[2] 显示: /dev/fb0 %dx%d\n",
	       (int)lv_display_get_horizontal_resolution(disp),
	       (int)lv_display_get_vertical_resolution(disp));

	/* 给顶层图层设统一字体（中文 + 符号 fallback）。
	 *
	 * ★ 必须放在 lv_linux_fbdev_create() 【之后】：
	 *   lv_layer_top() 返回的是"当前 display 的顶层图层"，
	 *   没有 display 时它是 NULL。我在 lv_init() 后面就调过一次，
	 *   结果日志报 "no display registered to get its top layer"，
	 *   字体没设上，键盘符号依然空白 —— 白折腾一轮。
	 *
	 * 为什么需要单独设：lv_msgbox_create(NULL) 的遮罩和密码键盘
	 * 都挂在 layer_top 上，不是任何 screen 的子对象，
	 * 拿不到 screen 上的字体设置。 */
	ui_apply_font(lv_layer_top());

	/* ---------- 3. 输入：evdev ---------- */
	for (i = 0; i < 10; i++) {
		snprintf(touch_path, sizeof(touch_path), "/dev/input/event%d", i);
		if (!has_abs_axis(touch_path))
			continue;

		/* lv_evdev_create 内部已经建好 indev 并设了 read_cb，
		 * 不要再自己 lv_indev_create，否则会多一个没有 read_cb
		 * 的空设备，日志里一直刷 "indev_read_cb is not registered"。 */
		if (lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path))
			printf("[3] 触摸: %s\n", touch_path);
		else
			printf("[3] 警告: lv_evdev_create 失败 (%s)\n", touch_path);
		break;
	}
	if (!touch_path[0])
		printf("[3] 警告: 没找到带 ABS 轴的触摸设备\n");

	tick_poll();

	/* ---------- 4. 界面 ----------
	 * 主页是整个导航栈的栈底：其它页面都是从这里 push 出去的，
	 * 所以必须先 ui_nav_init 登记好，再 lv_screen_load 显示。
	 */

	/* WiFi 扫描 + 连接模块要先初始化（建互斥锁 + 清零状态）。
	 * 必须在任何 lv_timer 可能触发它们之前完成。 */
	/* WiFi 扫描 + 连接模块要先初始化（建互斥锁 + 清零状态）。
	 * 必须在任何 lv_timer 可能触发它们之前完成。
	 *
	 * ★★ 顺序有讲究：timesync_init() 必须排在 wifi_conn_init() 前面。
	 *
	 *   wifi_conn_init() 内部会调 load_current_conn()，而如果
	 *   程序启动时 WiFi 已经是通的，它会立刻 timesync_start()
	 *   发起一次校时。这时如果 timesync 还没 init，
	 *   后面的 timesync_init() 会把 state 清零、busy 置回 false，
	 *   正在跑的校时线程就"失去登记"了 —— 于是下一次巡检
	 *   看到 state==IDLE 又发起一次，两次校时并发跑，
	 *   都 settimeofday，日志里就出现两条"校时成功"。
	 *   （实测确实是这个现象，加了诊断才定位到）
	 *
	 *   通用规则：模块的 init 一定在"可能被使用"之前，
	 *   而不是"按功能分组放在一起"。 */
	timesync_init();
	wifi_scan_init();
	wifi_conn_init();
	weather_init();
	netcheck_init();
	printf("[4] WiFi: %s  时间: %s\n",
	       wifi_available() ? "wlan0 就绪" : "wlan0 不存在",
	       timesync_trusted() ? "已同步" : "待校时");

	home = ui_page_home();

	ui_nav_init(home);
	lv_screen_load(home);
	printf("[4] 界面已加载（主页 + 3 个子页）\n");

	/* ---------- 4.5 唤醒框架（C++ 侧，见 integration/lv_wake_bridge.cpp） ----------
	 * ★ LVGL 是单线程的：业务线程只写 atomic 快照，
	 *   这里的 lv_timer 每 100ms 拉一次，内容变了才刷 label。
	 *   label 挂在 layer_top 上，不参与主页布局，删掉也不影响任何页面。 */
	wake_bridge_init();

	/*
	 * ★ 这个绿色状态标签（"chat:Stopping r=0"）【默认不显示】。
	 *
	 *   它挂在 lv_layer_top 上、对齐 TOP_LEFT (2,2)，正好压住页面标题
	 *   "回声伙伴" —— 每个页面都会被它盖住一截，看起来像 UI 错乱。
	 *   之前一直显示着，是因为我做语音链路调试时图方便。
	 *
	 *   保留代码、加环境变量控制：
	 *       LVGL_DEBUG_LABEL=1 ./lvgl_app      # 调试语音状态时才开
	 *   平时不设这个变量，界面干净。
	 *
	 *   wake_bridge_init() 照常调用 —— 唤醒功能本身和这个标签无关，
	 *   标签只是状态的显示窗口。不 attach 就不会有人去刷它。
	 */
	env = getenv("LVGL_DEBUG_LABEL");
	if (env && env[0] == '1') {
		lv_obj_t *lbl_wake = lv_label_create(lv_layer_top());

		lv_obj_set_style_text_font(lbl_wake, &lv_font_montserrat_14, 0);
		lv_obj_set_style_text_color(lbl_wake, lv_color_hex(0x00FF00), 0);
		lv_obj_set_style_bg_color(lbl_wake, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(lbl_wake, LV_OPA_50, 0);
		lv_obj_set_style_pad_all(lbl_wake, 2, 0);
		lv_label_set_text(lbl_wake, "wake:init");
		lv_obj_align(lbl_wake, LV_ALIGN_TOP_LEFT, 2, 2);
		wake_bridge_attach(lbl_wake);
		printf("[4.5] 语音状态标签已显示（LVGL_DEBUG_LABEL=1）\n");
	}

	/* 调试开关：LVGL_START_PAGE=net|netcheck|volume|about|camera|gallery 可以跳过点击，
	 * 启动后直接显示某个子页。
	 * 为什么需要：在 2.4 寸屏上用手指精确点中某一行很难，
	 * 改完 UI 想单独看网络页长什么样，有这个开关就不用反复试。
	 * 不设这个变量时行为完全不变（停在主页）。 */
	env = getenv("LVGL_START_PAGE");
	if (env && env[0]) {
		if (ui_goto(env) == 0) {
			printf("[4] 调试跳转到页面: %s\n", env);
			/* 强制立刻排布一次，否则下面的尺寸查询全是 0 */
			lv_obj_update_layout(lv_screen_active());
		} else {
			printf("[4] 警告: 未知页面名 '%s'（可用 home/net/netcheck/volume/about/camera/gallery）\n",
			       env);
		}
	}

	printf("[4] 当前页面: %dx%d\n",
	       (int)lv_obj_get_width(lv_screen_active()),
	       (int)lv_obj_get_height(lv_screen_active()));
	printf("\n");

	/* 自测模式：LVGL_SELFTEST=1 时把所有交互路径跑一遍再退回主页。
	 * 改完 UI 先跑这个，逻辑问题当场暴露，不用等人一格格点。 */
	env = getenv("LVGL_SELFTEST");
	if (env && env[0] == '1')
		ui_selftest();

	/* 调试开关：LVGL_SHOW_KEYBOARD=1 直接弹出密码键盘。
	 * 用途：验证键盘布局/字体时不用真的去点某个 AP
	 * （那会触发真实连接，改网络状态）。 */
	env = getenv("LVGL_SHOW_KEYBOARD");
	if (env && env[0] == '1') {
		extern void ui_wifi_psk_dialog_demo(void);

		ui_wifi_psk_dialog_demo();
		printf("[4] 调试：已弹出密码键盘\n");
	}

	/* ---------- 5. 主循环 ----------
	 * LVGL 9 推荐用 lv_timer_handler() 的返回值决定睡多久：
	 * 它返回"距离下次需要处理还有多少毫秒"。
	 * 固定 sleep 会要么太短（白耗 CPU）要么太长（触摸发钝）。
	 *
	 * 运行时长从 LVGL_RUN_SEC 读，【默认 0 = 一直跑】。
	 *
	 * ★ 这里原来默认是 60 秒，是个害人的默认值，必须写清楚为什么改：
	 *
	 *   这块屏的 fbcon（vtcon1）在启动前就被解绑了 —— 因为 fbcon 会
	 *   自己往 fb0 上画字符，和 LVGL 抢屏。解绑的代价是：
	 *   **一旦 lvgl_app 退出，屏幕上就再也没有任何东西会重绘**，
	 *   画面永远停在最后一帧，看起来就是"板子卡死了"。
	 *
	 *   我测试时习惯带 LVGL_RUN_SEC=60，跑完自动退；退完屏幕冻住，
	 *   下一次看板子就是"又卡住了"，实际上板子一直活着（SSH 正常、
	 *   load 0.02）。这个问题被误判过好几次。
	 *
	 *   所以默认改成常驻。真要做限时测试，显式传 LVGL_RUN_SEC=N。
	 */
	env = getenv("LVGL_RUN_SEC");
	run_sec = env ? strtol(env, NULL, 10) : 0;

	if (run_sec > 0)
		printf("[5] 预计运行 %ld 秒（LVGL_RUN_SEC=0 可常驻）\n\n", run_sec);
	else
		printf("[5] 一直运行\n\n");

	start = time(NULL);
	while (run_sec <= 0 || time(NULL) - start < run_sec) {
		uint32_t idle;
		time_t now = time(NULL);

		/* 兜底校时：每 60 秒看一眼"时间可信吗"。
		 *
		 * 为什么不能只靠 WiFi 连上那一刻触发：
		 *   开机时 WiFi 可能还没连上（比如热点没开、密码改了、
		 *   wpa_supplicant 自己还在重试），这时就不会有
		 *   "连接成功"事件，校时也就永远不会发生。
		 *   而 lvgl 是常驻的，可能一跑就是几天。
		 *
		 * 所以主循环里加一道巡检：不可信就试一次。
		 * timesync_start() 内部有 5 分钟幂等窗口，
		 * 而且自己有 g_busy 保护，这里每 60 秒调一次不会打爆线程。
		 *
		 * 用 now 判断而不是独立的计数器：now 是秒级时间，
		 * 校时成功后它会跳变，但这只影响巡检节奏，无害。 */
		if (now - last_time_check >= 60) {
			last_time_check = now;
			if (!timesync_trusted())
				timesync_start();
		}

		tick_poll();
		idle = lv_timer_handler();
		if (idle > 20)
			idle = 20;	/* 最多睡 20ms，保证触摸响应 */
		if (idle < 1)
			idle = 1;
		usleep(idle * 1000);
	}

	wake_bridge_deinit();
	printf("\n=== 退出 ===\n");
	return 0;
}
