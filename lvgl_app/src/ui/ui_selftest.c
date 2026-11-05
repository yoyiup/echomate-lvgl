/**
 * ui_selftest.c —— 自动回归测试：不需要人按屏幕
 *
 * 【为什么需要这个】
 * 在 2.4 寸屏上用手指点中特定一行很难，而且每次改完 UI
 * 都让人手动把 10 个交互路径点一遍，人会烦、也会漏。
 *
 * 这里用 lv_obj_send_event() 直接给控件发事件，走的是
 * 【和真实触摸完全相同的回调链】—— 区别只是事件源
 * 从 evdev 换成了代码。所以能验证逻辑，但不验证坐标映射
 * （那个要人按，见 readme 里的手工清单）。
 *
 * 用法：LVGL_SELFTEST=1 启动，跑完打印 PASS/FAIL 汇总后退回主页。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"
#include "../wifi/wifi_scan.h"
#include "../wifi/wifi_conn.h"

static int st_pass, st_fail;

#define CHECK(cond, msg) do {					\
	if (cond) { st_pass++; printf("  PASS  %s\n", msg); }	\
	else      { st_fail++; printf("  FAIL  %s\n", msg); }	\
} while (0)

/**
 * 推进 LVGL 时间 ms 毫秒
 *
 * 【为什么自测必须调这个】
 * LVGL 的页面切换动画、lv_timer 都不在发事件时同步执行，
 * 而是等主循环里 lv_timer_handler() 按 tick 推进才跑。
 *
 * lv_screen_load_anim() 在 time>0 时走动画路径：
 *   scr_load_internal()（真正改 act_scr）是在动画的 start_cb 里调的，
 *   而 start_cb 要等动画被 tick 驱动才执行。
 * 所以刚发完点击就查 lv_screen_active()，拿到的还是旧页面
 * —— 这是测试写法问题，不是代码 bug。真实用户 200ms 后就看到新页了。
 *
 * 这里手动喂 tick + 反复调 handler，模拟那 200ms 过去。
 */
static void pump(uint32_t ms)
{
	uint32_t step = 10;
	uint32_t t;

	for (t = 0; t < ms; t += step) {
		lv_tick_inc(step);
		lv_timer_handler();
	}
	/* 让布局也算一遍，方便后面查坐标/子对象 */
	lv_obj_update_layout(lv_screen_active());
}

/* 给某个对象的第 n 个子对象（深度 1）发点击 */
static void click_child(lv_obj_t *parent, int idx)
{
	lv_obj_t *c = lv_obj_get_child(parent, idx);

	if (c)
		lv_obj_send_event(c, LV_EVENT_CLICKED, NULL);
}

/**
 * 在 layer_top 上找当前打开的 msgbox
 *
 * lv_msgbox_create(NULL) 会先造一个全屏 backdrop 挂在 lv_layer_top()，
 * msgbox 是 backdrop 的孩子。所以要两级：top → backdrop → msgbox。
 */
static lv_obj_t *find_msgbox(void)
{
	lv_obj_t *top = lv_layer_top();
	uint32_t i, n = lv_obj_get_child_count(top);

	for (i = 0; i < n; i++) {
		lv_obj_t *backdrop = lv_obj_get_child(top, i);
		uint32_t j, m = lv_obj_get_child_count(backdrop);

		for (j = 0; j < m; j++) {
			lv_obj_t *c = lv_obj_get_child(backdrop, j);

			if (lv_obj_check_type(c, &lv_msgbox_class))
				return c;
		}
	}
	return NULL;
}

/* 点 msgbox footer 里文字为 text 的按钮 */
static bool click_msgbox_button(lv_obj_t *mbox, const char *text)
{
	lv_obj_t *footer = lv_msgbox_get_footer(mbox);
	uint32_t i, n;

	if (!footer)
		return false;

	n = lv_obj_get_child_count(footer);
	for (i = 0; i < n; i++) {
		lv_obj_t *btn = lv_obj_get_child(footer, i);
		uint32_t j, k = lv_obj_get_child_count(btn);

		for (j = 0; j < k; j++) {
			lv_obj_t *lb = lv_obj_get_child(btn, j);

			if (lv_obj_check_type(lb, &lv_label_class) &&
			    strcmp(lv_label_get_text(lb), text) == 0) {
				lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
				return true;
			}
		}
	}
	return false;
}

/* 找到 body 里文字等于 name 的那一行，返回行对象 */
/* 在一棵子树里找"名字等于 name 的行"
 *
 * ★ 为什么是递归而不是只查一层：
 *   主页的结构随功能演进在变 —— 最初 8 个行直接挂在 body 上，
 *   后来加了天气卡片，行就挪进了 body_list（body 的第 2 个子对象）。
 *   自测当时只查 body 的直接子对象，于是 4 项断言全挂，
 *   而界面本身是好的 —— 这是"测试和实现一起演化"时的典型陷阱：
 *   测试写死了内部结构，实现一变就假失败。
 *
 *   递归查找让测试只依赖"这行存在"这个契约，
 *   不依赖它在第几层，这样布局重构不会再误报。 */
static lv_obj_t *find_row(lv_obj_t *parent, const char *name)
{
	uint32_t i, n = lv_obj_get_child_count(parent);

	for (i = 0; i < n; i++) {
		lv_obj_t *row = lv_obj_get_child(parent, i);
		lv_obj_t *lb = lv_obj_get_child(row, 0);

		/* 先看自己是不是目标行 */
		if (lb && lv_obj_check_type(lb, &lv_label_class) &&
		    strcmp(lv_label_get_text(lb), name) == 0)
			return row;

		/* 再看子树里有没有（限制深度，避免在深列表上白跑） */
		if (lv_obj_get_child_count(row) > 0) {
			lv_obj_t *hit = find_row(row, name);

			if (hit)
				return hit;
		}
	}
	return NULL;
}

/* 找到一行里的开关 */
static lv_obj_t *row_switch(lv_obj_t *row)
{
	uint32_t i, n = lv_obj_get_child_count(row);

	for (i = 1; i < n; i++) {
		lv_obj_t *c = lv_obj_get_child(row, i);

		if (lv_obj_check_type(c, &lv_switch_class))
			return c;
	}
	return NULL;
}

void ui_selftest(void)
{
	lv_obj_t *home = ui_page_home();
	lv_obj_t *body = ui_screen_body(home);
	lv_obj_t *row, *sw;

	printf("\n========== 自测开始 ==========\n");

	/* ---------- 1. 主页开关改写 g_settings ---------- */
	printf("[1] 主页开关 -> g_settings\n");
	row = find_row(body, "唤醒词");
	CHECK(row != NULL, "找到「唤醒词」行");
	sw = row ? row_switch(row) : NULL;
	CHECK(sw != NULL, "该行有开关控件");
	if (sw) {
		bool before = g_settings.wake_word;

		/* 用 SEND_EVENT 模拟手指拨动：先翻状态再发 VALUE_CHANGED，
		 * 这两步正是真实触摸时 LVGL 内部做的。 */
		if (before)
			lv_obj_remove_state(sw, LV_STATE_CHECKED);
		else
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);

		CHECK(g_settings.wake_word == !before,
		      "拨动后 g_settings.wake_word 同步翻转");
	}

	/* ---------- 2. 跳转到网络页 ---------- */
	printf("[2] 主页 -> 网络页\n");
	row = find_row(body, "网络连接");
	CHECK(row != NULL, "找到「网络连接」行");
	if (row) {
		lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
		/* 切页是同步的（动画已关），给一帧时间让布局算完 */
		pump(50);
		CHECK(lv_screen_active() == ui_page_net(),
		      "点击后当前页面变成网络页");
	}

	/* ---------- 3. 网络页的 AP 列表 ----------
	 * ★ 这里刻意【不点】AP 行。
	 *   点它现在会真的发起 WiFi 连接（改密码、断网、重连），
	 *   自测不应该有这种副作用 —— 那是"改状态"，不是"验证逻辑"。
	 *   所以只验证列表结构正确（有行、行里有 SSID 文本）。
	 *   真正的连接流程靠人点一次，见 readme 的手工清单。
	 */
	printf("[3] 网络页 AP 列表结构\n");
	{
		lv_obj_t *nb = ui_screen_body(ui_page_net());
		lv_obj_t *list = NULL;
		uint32_t i, n = lv_obj_get_child_count(nb);

		/* body 里第 0 个是状态卡，第 1 个是 AP 列表容器 */
		for (i = 0; i < n; i++) {
			lv_obj_t *c = lv_obj_get_child(nb, i);

			if (c != lv_obj_get_child(nb, 0))
				list = c;
		}
		CHECK(list != NULL, "找到 AP 列表容器");

		if (list) {
			uint32_t rows = lv_obj_get_child_count(list);

			printf("      列表里有 %u 行\n", rows);
			/* 有 WiFi 环境就应该扫到东西；扫不到也不判失败
			 * （板子可能放在没 AP 的地方），只打印 */
			if (rows > 0) {
				lv_obj_t *row0 = lv_obj_get_child(list, 0);
				lv_obj_t *name = lv_obj_get_child(row0, 0);

				CHECK(name && lv_obj_check_type(name, &lv_label_class) &&
				      lv_label_get_text(name)[0] != '\0',
				      "第一行有非空 SSID 文本");
				printf("      第一行 SSID = %s\n",
				       name ? lv_label_get_text(name) : "?");
			}
		}
	}

	/* ---------- 4. 返回上一页 ---------- */
	printf("[4] 网络页 -> 返回\n");
	ui_nav_back();
	pump(50);
	CHECK(lv_screen_active() == home, "返回后回到主页");

	/* ---------- 5. 音量页滑块 ---------- */
	printf("[5] 音量页滑块\n");
	if (ui_goto("volume") == 0) {
		lv_obj_t *vb = ui_screen_body(ui_page_volume());

		CHECK(lv_screen_active() == ui_page_volume(), "跳到音量页");

		/* 滑块是 body 里唯一的 slider 控件，遍历找它 */
		uint32_t i, n = lv_obj_get_child_count(vb);
		lv_obj_t *sl = NULL;

		for (i = 0; i < n; i++) {
			lv_obj_t *c = lv_obj_get_child(vb, i);

			if (lv_obj_check_type(c, &lv_slider_class)) {
				sl = c;
				break;
			}
		}
		CHECK(sl != NULL, "音量页里有滑块");

		if (sl) {
			lv_slider_set_value(sl, 30, LV_ANIM_OFF);
			lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
			CHECK(g_settings.volume == 30,
			      "拖到 30 后 g_settings.volume == 30");
		}
	}

	/* ---------- 6. 关于页读系统信息 ---------- */
	printf("[6] 关于页\n");
	if (ui_goto("about") == 0) {
		lv_obj_t *ab = ui_screen_body(ui_page_about());
		uint32_t i, n = lv_obj_get_child_count(ab);
		int found_kernel = 0;

		CHECK(lv_screen_active() == ui_page_about(), "跳到关于页");

		/* 找"内核"那一行，看右边值是不是真内核版本（含 '.'） */
		for (i = 0; i < n; i++) {
			lv_obj_t *r = lv_obj_get_child(ab, i);
			lv_obj_t *k = lv_obj_get_child(r, 0);
			lv_obj_t *v = lv_obj_get_child(r, 1);

			if (k && v && lv_obj_check_type(k, &lv_label_class) &&
			    strcmp(lv_label_get_text(k), "内核") == 0) {
				const char *txt = lv_label_get_text(v);

				found_kernel = strlen(txt) > 3 &&
					       strchr(txt, '.') != NULL;
				printf("      内核值 = %s\n", txt);
				break;
			}
		}
		CHECK(found_kernel, "关于页「内核」显示的是真实版本号");
	}

	/* ---------- 7. 恢复出厂设置弹窗 ----------
	 * 完整路径是两步：点行 → 弹对话框 → 点「确认重置」。
	 * 只点第一步时 g_settings 不会变（那时只是弹窗），
	 * 所以必须再点确认按钮，才真正验证到重置逻辑。
	 */
	printf("[7] 恢复出厂设置弹窗\n");
	lv_screen_load(home);
	row = find_row(body, "恢复出厂设置");
	CHECK(row != NULL, "找到「恢复出厂设置」行");
	if (row) {
		lv_obj_t *mbox;

		g_settings.wake_word = false;	/* 先弄脏，验证重置真的改了 */
		g_settings.volume = 3;

		lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
		pump(100);

		mbox = find_msgbox();
		CHECK(mbox != NULL, "点击后弹出确认对话框");

		if (mbox) {
			CHECK(click_msgbox_button(mbox, "确认重置"),
			      "对话框里有「确认重置」按钮");
			pump(100);

			CHECK(g_settings.wake_word == true,
			      "重置后 wake_word 回默认值 true");
			CHECK(g_settings.volume == 60,
			      "重置后 volume 回默认值 60");
			CHECK(find_msgbox() == NULL,
			      "点确认后对话框已关闭（不会挡住界面）");
		}
	}

	/* 顺便验一下「取消」不会改数据 */
	printf("[8] 取消不生效\n");
	lv_screen_load(home);
	row = find_row(body, "恢复出厂设置");
	if (row) {
		lv_obj_t *mbox;

		g_settings.volume = 42;
		lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
		pump(100);
		mbox = find_msgbox();
		if (mbox) {
			click_msgbox_button(mbox, "取消");
			pump(100);
			CHECK(g_settings.volume == 42,
			      "点「取消」不修改任何设置");
			CHECK(find_msgbox() == NULL, "取消后对话框已关闭");
		}
	}

	/* ---------- 9. WiFi 模块 ---------- */
	printf("[9] WiFi 模块\n");
	{
		struct wifi_result wr;
		struct wifi_conn_info ci;
		int seq1, seq2;

		printf("      wlan0 存在: %d\n", wifi_available());
		CHECK(wifi_available() == 1, "wlan0 存在（驱动已加载）");

		/* 扫描模块：结果结构可读、seq 单调
		 * 注意这里不主动触发扫描（要 2 秒），只验证接口不崩 */
		seq1 = wifi_scan_copy(&wr);
		seq2 = wifi_scan_copy(&wr);
		CHECK(seq1 == seq2, "wifi_scan_copy 两次 seq 一致（没在扫）");
		printf("      已扫描 AP 数 = %d\n", wr.count);

		/* 连接模块：取状态不崩，state 在合法范围内 */
		wifi_conn_get(&ci);
		CHECK(ci.state >= WIFI_CONN_IDLE &&
		      ci.state <= WIFI_CONN_FAILED, "连接状态值合法");
		printf("      连接状态=%d stage=%s ssid=%s ip=%s\n",
		       ci.state, ci.stage, ci.ssid, ci.ip);

		/* 凭据查询：不存在的 SSID 必须返回 0，不能误判 */
		CHECK(wifi_conn_is_saved("不存在的网络-XXXX") == 0,
		      "未连过的 SSID 判定为无凭据");
	}

	/* ---------- 10. 点击 AP 行不丢 SSID（悬垂指针回归测试）----------
	 *
	 * 这是针对一个真实 bug 的回归测试：
	 * 原来 net_poll_cb 里用栈变量存扫描结果，事件 user_data 指向它，
	 * 回调时栈已失效 → SSID 变空 → wpa_cli 下发空 SSID → 连接必失败，
	 * 而界面只报"密码错误"，极具误导性。
	 *
	 * 判定依据：点击后弹窗里拿到的 SSID 必须和列表上显示的一致。
	 * 点击会走完整回调，可能发起真实连接（若该 AP 无保存凭据则先弹窗，
	 * 我们立刻把弹窗关掉，不会真的连）。 */
	printf("[10] 点击 AP 行不丢 SSID\n");
	{
		lv_obj_t *nb;
		lv_obj_t *list;
		uint32_t rows = 0;
		int wait;

		/* ★ 必须先切到网络页！
		 * net_poll_cb 开头就有 "不在网络页就直接返回" 的保护，
		 * 所以停在第 4 项切回的主页上时，列表永远不会被填充 ——
		 * 我第一版就是漏了这句，等了 6 秒列表还是空的。 */
		ui_goto("net");
		pump(100);

		nb = ui_screen_body(ui_page_net());

		/* 等扫描完成再点。
		 * 扫描是后台线程跑的（实测 1.66~2.12 秒），用 usleep 真等 ——
		 * 不能只 pump tick：扫描在另一个线程，靠真实时间流逝。
		 */
		for (wait = 0; wait < 30; wait++) {
			list = lv_obj_get_child(nb, 1);
			rows = list ? lv_obj_get_child_count(list) : 0;
			if (rows > 0)
				break;
			usleep(200 * 1000);
			pump(50);	/* 让 timer 有机会把结果搬进列表 */
		}

		if (rows == 0) {
			printf("      跳过（等了 6 秒仍没扫到 AP）\n");
		} else {
			uint32_t i;
			bool tested = false;

			/* 遍历找"需要密码"的那个（有保存凭据的不会弹窗，
			 * 测不到我们要验的东西）。第一个往往是已连过的
			 * 家里那只，所以必须往下找。 */
			for (i = 0; i < rows && !tested; i++) {
				lv_obj_t *row = lv_obj_get_child(list, i);
				lv_obj_t *name = lv_obj_get_child(row, 0);
				char shown[WIFI_SSID_MAX];
				char dlg_ssid[WIFI_SSID_MAX];

				snprintf(shown, sizeof(shown), "%s",
					 name ? lv_label_get_text(name) : "");
				if (!shown[0])
					continue;

				lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);

				/* 顺序要紧：先读弹窗 SSID，再关弹窗。
				 * 反过来读到的永远是空串。 */
				snprintf(dlg_ssid, sizeof(dlg_ssid), "%s",
					 ui_wifi_dialog_current_ssid());

				if (!dlg_ssid[0]) {
					/* 这个网有保存凭据，没弹窗，试下一个 */
					printf("      \"%s\" 有凭据未弹窗，继续\n", shown);
					continue;
				}

				printf("      列表=\"%s\"  弹窗=\"%s\"\n",
				       shown, dlg_ssid);
				ui_wifi_dialog_abort();

				CHECK(strcmp(shown, dlg_ssid) == 0,
				      "弹窗 SSID 与列表显示一致（悬垂指针已修）");
				tested = true;
			}

			if (!tested)
				printf("      跳过（所有 AP 都有保存凭据）\n");
		}
	}

	/* ---------- 回到主页 ---------- */
	lv_screen_load(home);
	CHECK(lv_screen_active() == home, "最终回到主页");

	/* ---------- 11. 滚动能力（防"能点不能滑"回归） ----------
	 *
	 * 这一节是给一个真实 bug 立的守门人：
	 * 主页列表曾经因为多写了一行
	 *     lv_obj_remove_flag(body_list, LV_OBJ_FLAG_SCROLLABLE)
	 * 而完全不能滑动，但点击一切正常、控制台零报错。
	 * 找根因花了很久，因为"能点"让人先怀疑触摸驱动，
	 * 而触摸驱动其实一个字节都没问题（抓 event1 有 123 个连续坐标）。
	 *
	 * 所以这里不测"滑动后内容动没动"（那要模拟手势，太重），
	 * 而是直接断言两个静态前提 —— 缺任何一个都滚不起来：
	 *   ① 容器有 SCROLLABLE 标志
	 *   ② 内容高度 > 可视高度（不然本来就没得滚，没有实际意义）
	 */
	printf("[11] 主页列表可滚动\n");
	{
		lv_obj_t *list = NULL;
		uint32_t i, n;

		/* body 的第 1 个孩子是天气卡片，第 2 个才是列表 */
		n = lv_obj_get_child_count(body);
		CHECK(n >= 2, "body 里有天气卡片 + 列表两个容器");
		if (n >= 2)
			list = lv_obj_get_child(body, 1);

		if (list) {
			CHECK(lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLLABLE),
			      "列表容器保留了 SCROLLABLE 标志");

			/* 必须先更新布局，否则拿到的尺寸全是 0
			 * （LVGL 是惰性布局，这个坑前面踩过） */
			lv_obj_update_layout(home);

			{
				int32_t vh = (int32_t)lv_obj_get_height(list);
				int32_t content = 0;
				uint32_t k, kn = lv_obj_get_child_count(list);

				/* LVGL 9 没有 lv_obj_get_scroll_height()，
				 * 自己把子项高度加起来 —— 反正都是等高的行。 */
				for (k = 0; k < kn; k++) {
					lv_obj_t *c = lv_obj_get_child(list, k);

					content += (int32_t)lv_obj_get_height(c);
				}

				printf("      列表可视高 %d，%u 行合计高 %d\n",
				       (int)vh, kn, (int)content);
				CHECK(content > vh,
				      "内容高于可视区（说明确实需要滚动）");
			}

			/* 真正滚一下，确认坐标会变 —— 这是最直接的证据 */
			lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
			lv_obj_update_layout(home);
			{
				int32_t y0 = (int32_t)lv_obj_get_scroll_y(list);

				lv_obj_scroll_by(list, 0, -80, LV_ANIM_OFF);
				lv_obj_update_layout(home);

				{
					int32_t y1 = (int32_t)lv_obj_get_scroll_y(list);

					printf("      滚动前 y=%d，滚 -80 后 y=%d\n",
					       (int)y0, (int)y1);
					CHECK(y1 != y0,
					      "调用 scroll_by 后 scroll_y 真的变了");
				}
				/* 复原，别让后面的截图带着偏移 */
				lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
			}
		}
	}

	printf("========== 自测完成：%d 通过 / %d 失败 ==========\n\n",
	       st_pass, st_fail);
}
