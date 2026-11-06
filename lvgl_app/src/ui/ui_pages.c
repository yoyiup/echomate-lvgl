/**
 * ui_pages.c —— 三个子页：网络连接 / 音量 / 固件版本
 *
 * 三个页面各有不同的"技术点"，正好覆盖三种常见交互：
 *   网络页  —— 点击后延迟生效（lv_timer 做一次性延时）+ 多目标互斥选择
 *   音量页  —— 滑块双向同步（控件 → 全局设置 → 另一个页面）
 *   关于页  —— 从应用层读真实系统信息（uname / /proc），并定时刷新
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "ui.h"
#include "../wifi/wifi_scan.h"
#include "../wifi/wifi_conn.h"
#include "../wifi/hotspot_control.h"

/* 前置声明：net_make_ap_row 里要引用它，而它定义在后面。
 * C 是单遍扫描的，不声明就会隐式声明成 int f() 引发类型冲突。 */
static void net_ap_clicked_cb(lv_event_t *e);
static void net_psk_done_cb(const char *ssid, const char *psk);

/* ============================================================
 * 网络连接页 —— 真实 WiFi 扫描（调 iw，后台线程）
 * ============================================================ */

static lv_obj_t *page_net;
static lv_obj_t *net_card_title;	/* 状态卡：大标题 */
static lv_obj_t *net_card_hint;		/* 状态卡：小字说明 */
static lv_obj_t *net_list;		/* AP 列表容器（动态重建）*/
static int net_last_seq = -1;		/* 上次看到的扫描序号 */

/**
 * ★★★ 扫描结果的持久副本
 *
 * 【为什么必须存在这里，不能留在 net_poll_cb 的局部变量里】
 *
 * 事件回调的 user_data 生命周期必须比控件长。原来的写法是：
 *     static void net_poll_cb(...) {
 *         struct wifi_result r;          // ← 栈变量
 *         wifi_scan_copy(&r);
 *         net_rebuild_list(&r);          // ← 里面把 &r.aps[i] 塞进事件
 *     }                                  // ← 函数返回，r 失效！
 *
 * 点击时再读 ap->ssid 就是读野内存。实测症状非常诡异：
 *     [网络] 点击 小桃 (-48 dBm, 加密)     ← 这儿还是对的（栈没被覆盖）
 *     [WiFi] 弹出密码输入框:               ← 变空了（栈已被覆盖）
 * 同一个指针，两行日志两个结果 —— 中间只隔了一次
 * wifi_conn_connect_saved() 调用，而它内部的 run_cmd() 用了
 * `char out[2048]` 的大局部数组，正好覆盖了刚才那个栈帧。
 *
 * 更毒的是：SSID 变成空串后，wpa_cli 下发的 ssid hex 是空，
 * 连接必然失败，但界面只显示"关联超时/密码错误"，
 * 让人以为是密码问题 —— 我一开始也差点往密码方向查。
 *
 * 所以：结果必须放静态区（生命周期 = 进程），事件只传下标。
 */
static struct wifi_result g_net_result;

/* 信号强度换颜色：绿=强 黄=中 红=弱。
 * 用颜色而不是 ▂▄▆█ 这类字符，是因为字模只含 ASCII + 中文，
 * 那些块状符号没有字形，会渲染成空白。
 */
static lv_color_t signal_color(int dbm)
{
	if (dbm >= -60)
		return lv_color_hex(0x5FD08A);	/* 强 */
	if (dbm >= -75)
		return lv_color_hex(0xE8C060);	/* 中 */
	return lv_color_hex(0xD97070);		/* 弱 */
}

/* 一个 AP 行：SSID（可省略号截断）+ 右侧"锁 -58"
 *
 * 注意参数是【下标】而不是指针 —— 见 g_net_result 上面的长注释。 */
static void net_make_ap_row(lv_obj_t *parent, int idx)
{
	const struct wifi_ap *ap = &g_net_result.aps[idx];
	lv_obj_t *row = lv_obj_create(parent);

	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 40);
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

	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, ap->ssid);
	lv_obj_set_style_text_color(name, UI_C_TEXT, 0);
	/* ★ grow=1 让 SSID 吃掉剩余宽度，把右边的强度推到最右。
	 * 但光有 grow 不够：长 SSID 会把行撑破。
	 * LV_LABEL_LONG_DOT 让超长文本自动加省略号。 */
	lv_obj_set_flex_grow(name, 1);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

	/* 右侧：加密标记 + 信号 dBm */
	lv_obj_t *info = lv_label_create(row);

	if (ap->secure)
		lv_label_set_text_fmt(info, "锁 %d", ap->signal);
	else
		lv_label_set_text_fmt(info, "%d", ap->signal);
	lv_obj_set_style_text_color(info, signal_color(ap->signal), 0);

	/* 事件只传下标；回调里再用 &g_net_result.aps[idx] 取数据 */
	lv_obj_add_event_cb(row, net_ap_clicked_cb, LV_EVENT_CLICKED,
			    (void *)(lv_intptr_t)idx);
}

/**
 * 点某个 AP：真的去连
 *
 * 两条路径：
 *   1. 这个 SSID 之前连过（wpa_supplicant 里有凭据）→ 免密直连
 *   2. 没连过 → 弹密码键盘
 *   开放网络（没加密）→ 直接连，不用密码
 */
static void net_ap_clicked_cb(lv_event_t *e)
{
	if (hotspot_is_running()) {
		ui_status("请先关闭手机热点");
		return;
	}
	int idx = (int)(lv_intptr_t)lv_event_get_user_data(e);
	const struct wifi_ap *ap;

	/* 下标校验：列表被重建后下标可能越界（比如扫描结果变少了，
	 * 而用户恰好在重建的瞬间点了旧行）。越界就读到别的内存，
	 * 比"点不中"危险得多，所以这里必须挡一道。 */
	if (idx < 0 || idx >= g_net_result.count) {
		printf("[网络] 点击的下标 %d 越界（当前 %d 个 AP），忽略\n",
		       idx, g_net_result.count);
		return;
	}
	ap = &g_net_result.aps[idx];

	printf("[网络] 点击 %s (%d dBm, %s)\n", ap->ssid, ap->signal,
	       ap->secure ? "加密" : "开放");

	/* 开放网络：直接连 */
	if (!ap->secure) {
		ui_status("正在连接 %s", ap->ssid);
		wifi_conn_start(ap->ssid, NULL);
		return;
	}

	/* 连过的网络：凭据已在 wpa_supplicant 里，免密直连 */
	if (wifi_conn_connect_saved(ap->ssid)) {
		printf("[网络] %s 有保存的凭据，直接连\n", ap->ssid);
		ui_status("正在连接 %s（免密）", ap->ssid);
		return;
	}

	/* 没连过：弹密码键盘 */
	printf("[网络] %s 需要密码，弹键盘\n", ap->ssid);
	ui_wifi_psk_dialog(ap->ssid, net_psk_done_cb);
}

/* 密码输入完成（或取消）后回到这里 */
static void net_psk_done_cb(const char *ssid, const char *psk)
{
	if (!psk) {
		ui_status("已取消");
		return;
	}
	printf("[网络] 收到密码，开始连接 %s\n", ssid);
	ui_status("正在连接 %s", ssid);
	wifi_conn_start(ssid, psk);
}

/* 按扫描结果重建整个列表
 *
 * ★ 先把结果拷进 g_net_result 再建行。
 *   行的 user_data 存的是 g_net_result 里的下标，
 *   所以列表必须和这份持久副本保持一致 —— 不能直接用传进来的 r。 */
static void net_rebuild_list(const struct wifi_result *r)
{
	int i;

	memcpy(&g_net_result, r, sizeof(g_net_result));

	/* 清空旧行。lv_obj_clean 删所有子对象，
	 * 比逐个 lv_obj_delete 省事，也不会漏。 */
	lv_obj_clean(net_list);

	if (r->err[0]) {
		lv_obj_t *lb = lv_label_create(net_list);

		lv_label_set_text_fmt(lb, "扫描出错: %s", r->err);
		lv_obj_set_style_text_color(lb, lv_color_hex(0xD97070), 0);
		lv_obj_set_style_pad_all(lb, 10, 0);
		return;
	}

	if (r->count == 0) {
		lv_obj_t *lb = lv_label_create(net_list);

		lv_label_set_text(lb, "没扫到网络（附近没有 AP？）");
		lv_obj_set_style_text_color(lb, UI_C_DIM, 0);
		lv_obj_set_style_pad_all(lb, 10, 0);
		return;
	}

	for (i = 0; i < r->count; i++)
		net_make_ap_row(net_list, i);
}

/* "重新扫描"按钮 */
static void net_rescan_cb(lv_event_t *e)
{
	(void)e;
	if (hotspot_is_running()) {
		ui_status("请先关闭手机热点");
		return;
	}
	wifi_scan_start();
	ui_status("正在扫描...");
}

/* "断开"按钮 */
static void net_disconnect_cb(lv_event_t *e)
{
	struct wifi_conn_info ci;

	(void)e;
	if (hotspot_is_running()) {
		ui_status("请在手机传文件页关闭热点");
		return;
	}

	wifi_conn_get(&ci);
	if (ci.state == WIFI_CONN_CONNECTED || ci.state == WIFI_CONN_WORKING) {
		wifi_conn_disconnect();
		ui_status("已断开");
	} else {
		ui_status("当前没有连接");
	}
}

static void net_hotspot_cb(lv_event_t *e)
{
	(void)e;
	ui_nav_push(ui_page_hotspot());
}

/**
 * 轮询扫描 + 连接状态（"下半部"）
 *
 * 两条线各用一个 seq 判断变化，变了才刷新 UI ——
 * 不能每 200ms 都重建列表，那样会疯狂创建/销毁对象，
 * 内存和 CPU 都白烧，界面还会闪。
 *
 * 页面不可见时直接跳过：timer 一直在跑，但不在网络页时
 * 没必要做任何事。
 */
static void net_poll_cb(lv_timer_t *t)
{
	static int last_conn_seq = -1;
	static int last_hotspot = -1;
	struct wifi_conn_info ci;
	struct wifi_result r;

	(void)t;

	if (lv_screen_active() != page_net)
		return;
	if (hotspot_is_running()) {
		if (last_hotspot != 1) {
			last_hotspot = 1;
			lv_label_set_text(net_card_title, "手机热点已开启");
			lv_label_set_text(net_card_hint, "点下方「手机传文件」管理");
		}
		return;
	}
	if (last_hotspot == 1) {
		last_hotspot = 0;
		last_conn_seq = -1;
		wifi_scan_start();
	}

	/* ---------- 先看连接状态（优先级更高）---------- */
	wifi_conn_get(&ci);

	if (ci.seq != last_conn_seq) {
		last_conn_seq = ci.seq;

		switch (ci.state) {
		case WIFI_CONN_CONNECTED:
			lv_label_set_text_fmt(net_card_title, "已连接 %s", ci.ssid);
			lv_label_set_text_fmt(net_card_hint, "IP %s", ci.ip);
			lv_obj_set_style_text_color(net_card_title,
						    lv_color_hex(0x5FD08A), 0);
			printf("[网络] UI 显示已连接: %s / %s\n", ci.ssid, ci.ip);
			break;
		case WIFI_CONN_WORKING:
			lv_label_set_text_fmt(net_card_title, "正在连接 %s", ci.ssid);
			lv_label_set_text(net_card_hint, ci.stage);
			lv_obj_set_style_text_color(net_card_title, UI_C_ACCENT, 0);
			break;
		case WIFI_CONN_FAILED:
			lv_label_set_text(net_card_title, "连接失败");
			lv_label_set_text_fmt(net_card_hint, "%s：%s",
					      ci.stage, ci.err);
			lv_obj_set_style_text_color(net_card_title,
						    lv_color_hex(0xD97070), 0);
			break;
		default:
			lv_label_set_text(net_card_title, "未连接");
			lv_label_set_text(net_card_hint, "点下方名称连接（点重扫刷新）");
			lv_obj_set_style_text_color(net_card_title, UI_C_TEXT, 0);
			break;
		}
	}

	/* ---------- 再看扫描结果 ---------- */
	wifi_scan_copy(&r);

	if (r.scanning)
		return;

	if (r.seq == net_last_seq)
		return;		/* 没新结果 */

	net_last_seq = r.seq;

	if (!wifi_available()) {
		lv_label_set_text(net_card_title, "无线未就绪");
		lv_label_set_text(net_card_hint, "wlan0 不存在，先加载 r8723bs.ko");
		lv_obj_clean(net_list);
		return;
	}

	/* 扫描完成只更新列表，不动状态卡 ——
	 * 状态卡归"连接状态"用，两条信息不要互相覆盖。 */
	net_rebuild_list(&r);
	printf("[网络] 扫描完成：%d 个 AP\n", r.count);
}

static lv_obj_t *build_net(void)
{
	lv_obj_t *scr = ui_make_screen("网络连接", true);
	lv_obj_t *body = ui_screen_body(scr);

	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	/* ---- 顶部状态卡（显示连接状态）---- */
	lv_obj_t *card = lv_obj_create(body);
	lv_obj_set_size(card, lv_pct(100), 56);
	lv_obj_set_style_bg_color(card, UI_C_BAR, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_radius(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 10, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

	/* 文字区限宽 186，右边留给两个按钮。不限宽的话长 SSID
	 * 会顶到按钮上，两层内容叠在一起看不清。 */
	net_card_title = lv_label_create(card);
	lv_label_set_text(net_card_title, "未连接");
	lv_obj_set_width(net_card_title, 186);
	lv_label_set_long_mode(net_card_title, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(net_card_title, UI_C_TEXT, 0);
	lv_obj_align(net_card_title, LV_ALIGN_LEFT_MID, 0, -10);

	net_card_hint = lv_label_create(card);
	lv_label_set_text(net_card_hint, "点下方名称连接");
	lv_obj_set_width(net_card_hint, 186);
	lv_label_set_long_mode(net_card_hint, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(net_card_hint, UI_C_DIM, 0);
	lv_obj_align(net_card_hint, LV_ALIGN_LEFT_MID, 0, 10);

	/* 右上：重扫按钮 */
	lv_obj_t *btn = lv_button_create(card);
	lv_obj_set_size(btn, 58, 26);
	lv_obj_set_style_bg_color(btn, UI_C_ACCENT, 0);
	lv_obj_set_style_radius(btn, 6, 0);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, -15);
	lv_obj_add_event_cb(btn, net_rescan_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *bl = lv_label_create(btn);
	lv_label_set_text(bl, "重扫");
	lv_obj_set_style_text_color(bl, lv_color_hex(0x0A1628), 0);
	lv_obj_center(bl);

	/* 右下：断开按钮 */
	lv_obj_t *dbtn = lv_button_create(card);
	lv_obj_set_size(dbtn, 58, 26);
	lv_obj_set_style_bg_color(dbtn, lv_color_hex(0x3A4A6A), 0);
	lv_obj_set_style_radius(dbtn, 6, 0);
	lv_obj_align(dbtn, LV_ALIGN_RIGHT_MID, 0, 15);
	lv_obj_add_event_cb(dbtn, net_disconnect_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *dl = lv_label_create(dbtn);
	lv_label_set_text(dl, "断开");
	lv_obj_set_style_text_color(dl, UI_C_TEXT, 0);
	lv_obj_center(dl);

	/* 与主页相同的入口，网络页也能直接进入热点管理。 */
	lv_obj_t *hotspot_btn = lv_button_create(body);
	lv_obj_set_size(hotspot_btn, lv_pct(100), 30);
	lv_obj_set_style_bg_color(hotspot_btn, UI_C_ACCENT, 0);
	lv_obj_add_event_cb(hotspot_btn, net_hotspot_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *hotspot_text = lv_label_create(hotspot_btn);
	lv_label_set_text(hotspot_text, "手机传文件 / 热点");
	lv_obj_set_style_text_color(hotspot_text, lv_color_hex(0x0A1628), 0);
	lv_obj_center(hotspot_text);

	/* ---- AP 列表（可滚动，动态填充）---- */
	net_list = lv_obj_create(body);
	lv_obj_set_width(net_list, lv_pct(100));
	lv_obj_set_flex_grow(net_list, 1);
	lv_obj_set_style_bg_color(net_list, UI_C_LIST, 0);
	lv_obj_set_style_border_width(net_list, 0, 0);
	lv_obj_set_style_radius(net_list, 0, 0);
	lv_obj_set_style_pad_all(net_list, 0, 0);
	lv_obj_set_style_pad_row(net_list, 0, 0);
	lv_obj_set_flex_flow(net_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(net_list, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(net_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(net_list, LV_SCROLLBAR_MODE_OFF);

	/* ---- 轮询 timer（"下半部"）：200ms 看一眼有没有新结果 ---- */
	lv_timer_create(net_poll_cb, 200, NULL);

	/* 进页面就扫一次 */
	if (!hotspot_is_running())
		wifi_scan_start();

	return scr;
}

lv_obj_t *ui_page_net(void)
{
	if (!page_net)
		page_net = build_net();
	return page_net;
}

/* ============================================================
 * 音量页
 * ============================================================ */

static lv_obj_t *page_volume;
static lv_obj_t *vol_slider;
static lv_obj_t *vol_value_lb;

static void vol_update_label(int v)
{
	lv_label_set_text_fmt(vol_value_lb, "%d", v);
}

/* 滑块拖动 / 点击轨道都会进这里 */
static void vol_slider_cb(lv_event_t *e)
{
	int v = (int)lv_slider_get_value(lv_event_get_target(e));

	g_settings.volume = v;		/* 写回唯一数据源 */
	vol_update_label(v);
}

/* 三个快捷按钮：静音 / 一半 / 最大 */
static void vol_preset_cb(lv_event_t *e)
{
	int v = (int)(lv_intptr_t)lv_event_get_user_data(e);

	/* 直接改滑块值，会触发 VALUE_CHANGED → vol_slider_cb
	 * 自动把 g_settings 和数字标签一并更新。
	 * 不要在这里再手动写一遍 g_settings，否则逻辑有两份，早晚不一致。 */
	lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);

	printf("[音量] 预设 -> %d\n", v);
	ui_status("音量设为 %d", v);
}

static lv_obj_t *build_volume(void)
{
	lv_obj_t *scr = ui_make_screen("音量", true);
	lv_obj_t *body = ui_screen_body(scr);

	/*
	 * 布局策略：不用 flex 排垂直位置，改成"每块自己 align"。
	 *
	 * 为什么放弃 flex：
	 *   body 是 COLUMN 流时，交叉轴设 START 会让 lv_pct(86) 的滑块
	 *   顶到左边（因为百分比相对父宽度算完还从左边开始摆）；
	 *   设 CENTER 又会让百分比相对内容宽度算，滑块变短。
	 *   两种都不对。而 body 里只有三块东西，位置固定，
	 *   用绝对对齐（LV_ALIGN_TOP_MID + y 偏移）最直观，
	 *   改一行就看得到效果，不用推 flex 的分配规则。
	 */

	/* ---- 大号数字：居中，避开标题栏 ----
	 *
	 * ★ transform_scale 的三个坑：
	 *  1) 只在绘制时放大，不改布局尺寸。16px 高放大 2 倍画成 32px，
	 *     布局框还是 16px —— 所以下面的内容要预留 32px 以上的空间。
	 *  2) 缩放以中心为基准，左右各扩一圈。靠左边界会被裁掉半个字。
	 *  3) text_align 对单行 label 无效：单行 label 的宽度自动收缩到
	 *     文字宽度，框再宽也不撑开，所以想居中要让 label 自己居中，
	 *     而不是指望框内的文字对齐。
	 *
	 * 结论：不设尺寸 + IGNORE_LAYOUT + 整体 LV_ALIGN_TOP_MID。
	 */
	vol_value_lb = lv_label_create(body);
	lv_obj_set_style_text_color(vol_value_lb, UI_C_ACCENT, 0);
	lv_obj_set_style_transform_scale(vol_value_lb, 512, 0);
	lv_obj_add_flag(vol_value_lb, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(vol_value_lb, LV_ALIGN_TOP_MID, 0, 14);
	vol_update_label(g_settings.volume);

	/* ---- 单位提示：绝对定位，跟在数字下方 ---- */
	lv_obj_t *unit = lv_label_create(body);
	lv_label_set_text(unit, "音量 0 - 100");
	lv_obj_set_style_text_color(unit, UI_C_DIM, 0);
	lv_obj_add_flag(unit, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 56);

	/* ---- 滑块 ----
	 * 初始值取全局设置，这样从主页进来看到的是上次调过的值。
	 * 宽度用固定 px 而不是 pct：px 的位置可预测，
	 * 320 - 40 = 280，左右各留 20px 边距。
	 */
	vol_slider = lv_slider_create(body);
	lv_obj_set_size(vol_slider, 280, 12);
	lv_slider_set_range(vol_slider, 0, 100);
	lv_slider_set_value(vol_slider, g_settings.volume, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(vol_slider, UI_C_LINE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(vol_slider, UI_C_ACCENT, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(vol_slider, UI_C_ACCENT, LV_PART_KNOB);
	lv_obj_add_event_cb(vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_flag(vol_slider, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(vol_slider, LV_ALIGN_TOP_MID, 0, 86);

	/* ---- 三个快捷按钮 ----
	 * 容器占满宽度，内部 ROW + SPACE_EVENLY 让三个按钮均分。
	 * flex_grow(1) 让每个按钮吃掉等分的宽度，间距自然均匀。
	 */
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

	static const struct {
		const char *txt;
		int val;
	} presets[] = {
		{ "静音", 0 },
		{ "50",  50 },
		{ "最大", 100 },
	};

	for (unsigned i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		lv_obj_t *b = lv_button_create(btns);
		lv_obj_set_flex_grow(b, 1);
		lv_obj_set_height(b, 32);
		lv_obj_set_style_bg_color(b, UI_C_BAR, 0);
		lv_obj_set_style_radius(b, 6, 0);

		lv_obj_t *l = lv_label_create(b);
		lv_label_set_text(l, presets[i].txt);
		lv_obj_set_style_text_color(l, UI_C_TEXT, 0);
		lv_obj_center(l);

		lv_obj_add_event_cb(b, vol_preset_cb, LV_EVENT_CLICKED,
				    (void *)(lv_intptr_t)presets[i].val);
	}

	return scr;
}

lv_obj_t *ui_page_volume(void)
{
	if (!page_volume)
		page_volume = build_volume();
	return page_volume;
}

/* ============================================================
 * 固件版本页（关于）
 * ============================================================ */

static lv_obj_t *page_about;
static lv_obj_t *about_uptime_lb;

/* 读 /proc/uptime 的秒数。
 * /proc/uptime 格式是 "123.45 67.89"，第一个数是开机至今秒数。
 * 应用层读 /proc 是 Linux 下最普通的做法 —— 这就是"一切皆文件"。 */
static long read_uptime_sec(void)
{
	FILE *f = fopen("/proc/uptime", "r");
	long sec = -1;
	double d;

	if (!f)
		return -1;
	if (fscanf(f, "%lf", &d) == 1)
		sec = (long)d;
	fclose(f);
	return sec;
}

/* 每秒刷一次运行时间。
 * 页面被切走时这个 timer 还在跑 —— 但它只改一个 label 的文字，
 * 不在屏幕上就几乎不耗资源。真要优化可以在页面切走时 pause，
 * 但为省这点开销引入页面生命周期回调，不值得。 */
static void about_tick_cb(lv_timer_t *t)
{
	long s = read_uptime_sec();
	(void)t;

	if (s < 0) {
		lv_label_set_text(about_uptime_lb, "运行时间: 读取失败");
		return;
	}
	lv_label_set_text_fmt(about_uptime_lb, "运行时间: %ld 分 %ld 秒",
			      s / 60, s % 60);
}

/* 一行"键: 值"，返回右侧那个值 label（便于后面改内容）*/
static lv_obj_t *about_make_row(lv_obj_t *parent, const char *key,
				const char *val)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, 32);
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

	lv_obj_t *k = lv_label_create(row);
	lv_label_set_text(k, key);
	lv_obj_set_style_text_color(k, UI_C_DIM, 0);

	lv_obj_t *v = lv_label_create(row);
	lv_label_set_text(v, val);
	lv_obj_set_style_text_color(v, UI_C_TEXT, 0);
	lv_obj_set_flex_grow(v, 1);
	/* 值右对齐：键左、值右，扫一眼就能对上 */
	lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);

	return v;
}

static lv_obj_t *build_about(void)
{
	lv_obj_t *scr = ui_make_screen("固件版本", true);
	lv_obj_t *body = ui_screen_body(scr);
	struct utsname u;
	char buf[64];

	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	/* uname() 是系统调用，拿的是内核的真实信息。
	 * 这行日志能直接证明"应用层确实摸到了内核"。 */
	if (uname(&u) == 0) {
		printf("[关于] uname -> %s %s\n", u.sysname, u.release);

		/* 型号只写英文名：中文 4 字 + "Echo-Mate" 在 320px 宽
		 * 减去"产品"两字和左右 padding 之后放不下，右侧会被裁掉。
		 * 中文名已经在标题栏出现过了，这里不必重复。 */
		about_make_row(body, "产品", "Echo-Mate");
		about_make_row(body, "固件版本", "v0.1.0-d8");

		/* release 形如 5.10.110，16px 字模下中文一行放得下 */
		about_make_row(body, "内核", u.release);
		about_make_row(body, "架构", u.machine);
	} else {
		about_make_row(body, "产品", "Echo-Mate");
		about_make_row(body, "固件版本", "v0.1.0-d8");
		about_make_row(body, "内核", "读取失败");
	}

	snprintf(buf, sizeof(buf), "%d.%d.%d",
		 lv_version_major(), lv_version_minor(), lv_version_patch());
	about_make_row(body, "LVGL", buf);

	about_make_row(body, "屏幕", "320x240 RGB565");
	about_make_row(body, "显示驱动", "echo_lcd_my (lcd.ko)");
	about_make_row(body, "触摸驱动", "echo_my_touch (touch.ko)");

	/* 这一行的值每秒钟会被 about_tick_cb 改写 */
	about_uptime_lb = about_make_row(body, "运行时间", "...");
	lv_timer_create(about_tick_cb, 1000, NULL);
	about_tick_cb(NULL);		/* 先立刻填一次，别让用户看到 "..." */

	return scr;
}

lv_obj_t *ui_page_about(void)
{
	if (!page_about)
		page_about = build_about();
	return page_about;
}
