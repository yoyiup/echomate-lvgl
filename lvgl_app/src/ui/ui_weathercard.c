/**
 * ui_weathercard.c —— 主页顶部的天气卡片
 *
 * 显示：城市 / 温度 / 天气 / 湿度 / 空气质量
 * 数据来自 HTTP 接口（t.weather.itboy.net），每 10 分钟刷新一次。
 *
 * 【为什么放在主页顶部】
 * 这块 320x240 的屏上，主页的列表本来就有 8 项要滚动。
 * 天气是"扫一眼就够"的信息，做成一条紧凑的横幅（44px）
 * 贴在标题栏下面最合适：不用滚动就能看到，也不挤占列表。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui.h"
#include "../net/weather.h"
#include "../net/timesync.h"

/* 广州。和风天气城市编码体系，广州 = 101280101。
 * 换城市只改这里。 */
#define DEFAULT_CITY_CODE "101280101"

/* 刷新间隔。天气数据本身 10 分钟才更新一次，
 * 我们也按 10 分钟拉，别拿这个接口当心跳用。 */
#define REFRESH_MS	(10 * 60 * 1000)
/* 还没拿到数据时的重试间隔：开机应用比 WiFi 先起，必须短周期自愈 */
#define RETRY_MS	(20 * 1000)

/*
 * 数据"过旧"的阈值（秒）。
 *
 * 这个接口（t.weather.itboy.net）实测更新很不规律：
 *   2026-11-01 凌晨整整停更 5 小时，白天也是 1~2 小时才更一次，
 *   而且 HTTP 头上带 Cache-Control: max-age=3000（CDN 缓存 50 分钟）。
 * 所以正常延迟本来就能到 1 小时以上，阈值设太小会天天误报。
 * 3 小时是"能容忍正常抖动，又不至于真停更还当没事"的折中。
 */
#define WX_STALE_SEC	(3 * 60 * 60)

static lv_obj_t *w_temp;	/* 大号温度 */
static lv_obj_t *w_type;	/* 天气（多云/晴）*/
static lv_obj_t *w_detail;	/* 湿度 / AQI / 温度区间 */
static lv_obj_t *w_clock;	/* 实时时钟（联网校时之后才可信）*/
static int w_last_seq = -1;

/* 不同天气给不同颜色，扫一眼就知道大概 */
static lv_color_t type_color(const char *type)
{
	if (strstr(type, "晴"))
		return lv_color_hex(0xF0C860);	/* 晴：暖黄 */
	if (strstr(type, "云") || strstr(type, "阴"))
		return lv_color_hex(0x9FB4CC);	/* 云/阴：灰蓝 */
	if (strstr(type, "雨"))
		return lv_color_hex(0x6FA8E8);	/* 雨：蓝 */
	if (strstr(type, "雪"))
		return lv_color_hex(0xD8E8F8);	/* 雪：白蓝 */
	if (strstr(type, "雾") || strstr(type, "霾"))
		return lv_color_hex(0xA89880);	/* 雾霾：土黄 */
	return UI_C_TEXT;
}

static void w_render(const struct weather_info *w)
{
	if (!w->valid) {
		lv_label_set_text(w_temp, "--");
		lv_obj_set_style_text_color(w_temp, UI_C_DIM, 0);
		lv_label_set_text(w_type, w->err[0] ? "获取失败" : "");
		lv_obj_set_style_text_color(w_type, lv_color_hex(0xD97070), 0);
		lv_label_set_text(w_detail, "");
		return;
	}

	lv_label_set_text_fmt(w_temp, "%d", w->temp);
	lv_obj_set_style_text_color(w_temp, UI_C_TEXT, 0);

	/*
	 * ★ 天气文字后面带上【数据时间】。
	 *
	 *   2026-11-01 用户问"这天气对吗，怎么好像一直是一样的"。
	 *   查下来不是程序的问题：接口 t.weather.itboy.net 本身停更了 ——
	 *   返回体里 date 还是昨天、cityInfo.updateTime 停在昨晚 20:08，
	 *   HTTP 头 Age: 2026 说明 CDN 缓存了 34 分钟。
	 *
	 *   但【界面上看不出来】才是真缺陷：我们拿到了 update_time 却
	 *   从来没显示过。数据停更时，用户只能看到"29℃ 多云"永远不变，
	 *   无从判断是"程序卡了"还是"数据源给了旧数据"。
	 *
	 *   所以显示成 "多云  11-01 00:41"：
	 *     · 和右上角实时时钟一对比，数据新不新一眼可辨
	 *     · 带日期，跨天也能看出来（只显示时分时，昨天 20:08 和
	 *       今天 20:08 长得一模一样，等于没提示）
	 *
	 *   字号没变、位置没变，只是多几个字符，layout 不受影响。
	 */
	/*
	 * 数据本身有多旧？
	 *
	 * ★ 用 update_epoch（weather.cpp 里用 mktime 算好的）而不是自己
	 *   解析字符串：时区已经在 timesync_setup_tz() 里统一成 CST-8，
	 *   mktime 出来的 epoch 和 time(NULL) 直接可比，不需要 +8。
	 *
	 * 阈值 3 小时：这个接口实测更新很不规律 ——
	 *   2026-11-01 凌晨它整整停更了 5 小时（date 还停在昨天），
	 *   白天恢复后也是一两个小时才更一次。3 小时能覆盖正常延迟，
	 *   又能在真停更时及时提示。
	 */
	{
		long age = 0;
		bool stale = false;

		if (w->update_epoch > 0) {
			age = (long)time(NULL) - w->update_epoch;
			stale = age > WX_STALE_SEC;
		}

		/*
		 * ★ 文字只给"天气 + 过旧警告"，不再拼更新时间和日期。
		 *
		 *   加桌面形象后卡片只剩约 226px 宽，原来"多云  11-02 10:16"
		 *   会被省略号截成"多云 11-02 1…"，看着像坏了。
		 *   而且右边已经有实时时钟（更准），更新时间的价值只剩
		 *   "这份数据是不是旧的"——那就只在【过旧】时才显示，
		 *   而且只显示时分，够用了。
		 */
		if (w->update_time[0] && stale) {
			const char *t = w->update_time;
			const char *hm = t;

			/* "11-02 10:16" 取后面那截时分；格式不符就直接用原文 */
			if (t[5] == ' ')
				hm = t + 6;

			lv_label_set_text_fmt(w_type, "%s %s !", w->type, hm);
			lv_obj_set_style_text_color(w_type,
						    lv_color_hex(0xD0A050), 0);
		} else if (w->type[0]) {
			lv_label_set_text(w_type, w->type);
			lv_obj_set_style_text_color(w_type,
						    type_color(w->type), 0);
		} else {
			lv_label_set_text(w_type, w->type);
			lv_obj_set_style_text_color(w_type,
						    type_color(w->type), 0);
		}
	}

	/* 湿度 + AQI + 今日温度区间，一行塞下 */
	if (w->aqi >= 0)
		lv_label_set_text_fmt(w_detail, "%s %d%%  AQI %d %s",
				      w->city, w->humidity, w->aqi, w->quality);
	else
		lv_label_set_text_fmt(w_detail, "%s %d%%  %d~%d℃",
				      w->city, w->humidity, w->low, w->high);
}

/* 每秒刷新一次时钟。
 *
 * ★ 这里有个诚实性问题：系统时间在没校时之前是不可信的 ——
 *   板子没有电池 RTC，上电时内核时间从编译时刻开始，
 *   可能停在 1970 也可能是几个月前的固件编译时间。
 *   直接把它显示出来，用户会以为"现在是这个点"，比不显示更糟。
 *
 *   所以判据分两档：
 *     - 已校时（timesync_trusted）→ 正常显示，白色
 *     - 未校时但时间看着合理（> 2025 年，说明有人手动设过）→ 显示但变灰
 *     - 未校时且时间明显不对 → 显示 "--:--:--"，悬停提示待校时
 *
 *   这样"联网校时到底有没有生效"在界面上是看得见的。 */
static void w_clock_update(void)
{
	time_t now = time(NULL);
	struct tm lt;
	bool trust = timesync_trusted() != 0;

	/* 2025-01-01 00:00:00 UTC 的 epoch。低于它的系统时间
	 * 一定是没校过的（固件不可能编在 2025 年以前）。 */
	if (!trust && now < 1735689600L) {
		lv_label_set_text(w_clock, "--:--:--");
		lv_obj_set_style_text_color(w_clock, lv_color_hex(0x4A5A70), 0);
		return;
	}

	localtime_r(&now, &lt);
	lv_label_set_text_fmt(w_clock, "%02d:%02d:%02d",
			      lt.tm_hour, lt.tm_min, lt.tm_sec);
	/* 已校时用亮白，未校时用灰 —— 一眼能看出这个时间靠不靠得住 */
	lv_obj_set_style_text_color(w_clock,
				    trust ? UI_C_TEXT : lv_color_hex(0x7A8A9E), 0);
}

static void w_clock_cb(lv_timer_t *t)
{
	(void)t;
	w_clock_update();
}

static void w_poll_cb(lv_timer_t *t)
{
	struct weather_info w;
	int seq;

	(void)t;

	seq = weather_get(&w);
	if (seq == w_last_seq)
		return;
	w_last_seq = seq;

	/* 正在拉且还没有数据时才显示"获取中"，
	 * 有旧数据的刷新过程中保持旧数据不闪 */
	if (weather_busy() && !w.valid) {
		lv_label_set_text(w_temp, "--");
		lv_label_set_text(w_type, "获取中");
		lv_obj_set_style_text_color(w_type, UI_C_DIM, 0);
		return;
	}

	w_render(&w);
}

/* 点击卡片：立刻刷新一次 */
static void w_click_cb(lv_event_t *e)
{
	(void)e;
	weather_fetch(DEFAULT_CITY_CODE);
	ui_status("正在刷新天气...");
}

/* 定时刷新（有数据时 10 分钟一次，没数据时 20 秒一次重试） */
static void w_refresh_timer_cb(lv_timer_t *t)
{
	struct weather_info w;

	weather_fetch(DEFAULT_CITY_CODE);

	/*
	 * ★ 为什么要自适应周期
	 *   开机时 lvgl_app 和 WiFi 是并行起来的，应用往往【比网络先就绪】，
	 *   第一次取天气必然失败。如果固定 10 分钟才重试，屏幕上就要挂
	 *   十分钟"获取失败"——用户看到的就是"WiFi 不是连上了吗，怎么没天气"。
	 *   所以没拿到数据就 20 秒后再来，拿到后恢复 10 分钟。
	 */
	weather_get(&w);
	lv_timer_set_period(t, w.valid ? REFRESH_MS : RETRY_MS);
}

/**
 * 在 parent 里放一张天气卡片
 *
 * 高度 40px：240 高的屏上每 1px 都影响列表能露出几行。
 * 40 刚好够"放大的温度 + 两行小字"（温度字 24px 视觉高 + 上下各 8px 余量）。
 * 第一版给 46 太奢侈，列表少露了半行。
 *
 * 返回卡片对象，方便调用方决定位置。
 */
lv_obj_t *ui_weather_card(lv_obj_t *parent)
{
	lv_obj_t *card = lv_obj_create(parent);

	lv_obj_set_size(card, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(card, lv_color_hex(0x1E3050), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_radius(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 8, 0);
	lv_obj_set_style_pad_ver(card, 4, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(card, w_click_cb, LV_EVENT_CLICKED, NULL);

	/*
	 * ★ 布局从"按 320 宽写死坐标"改成"自适应"。
	 *
	 *   原来是给通栏卡片量出来的固定偏移（温度 x=6、度数 34、文字 52、
	 *   明细宽 236、时钟贴右）—— 主页加了桌面形象之后卡片只剩 ~226 宽，
	 *   明细的 236 直接溢出，时钟就压在日期上了。
	 *
	 *   现在改成两层 flex：
	 *     card ─┬─ tblock 固定 46：温度 + 摄氏度符号（内部还是绝对定位，
	 *            │                 因为 transform_scale 放大后不能用布局算宽度）
	 *           └─ rcol flex_grow ─┬─ 第一行：天气文字 ←→ 时钟（两端对齐）
	 *                              └─ 第二行：城市/湿度/AQI（超长省略号）
	 *   这样卡片多宽都不会重叠。
	 */
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(card, 2, 0);

	/* ---- 左：温度块（固定宽）---- */
	lv_obj_t *tblock = lv_obj_create(card);

	lv_obj_remove_style_all(tblock);
	lv_obj_set_size(tblock, 46, lv_pct(100));
	lv_obj_remove_flag(tblock, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(tblock, LV_OBJ_FLAG_CLICKABLE);

	/* 大号温度：数字放大 1.5 倍（384/256）。
	 * transform_scale 只放大绘制、不改布局尺寸，且以中心为基准，
	 * 所以这里仍然用绝对定位，不参与 flex。 */
	w_temp = lv_label_create(tblock);
	lv_label_set_text(w_temp, "--");
	lv_obj_set_style_text_color(w_temp, UI_C_DIM, 0);
	lv_obj_set_style_transform_scale(w_temp, 384, 0);
	lv_obj_add_flag(w_temp, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(w_temp, LV_ALIGN_LEFT_MID, 2, -2);

	/* 摄氏度符号：紧跟温度右边。偏移是量出来的（放大后视觉宽约 27px） */
	lv_obj_t *deg = lv_label_create(tblock);

	lv_label_set_text(deg, "C");
	lv_obj_set_style_text_color(deg, UI_C_DIM, 0);
	lv_obj_add_flag(deg, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(deg, LV_ALIGN_LEFT_MID, 30, -9);

	/* ---- 右：两行文字列 ---- */
	lv_obj_t *rcol = lv_obj_create(card);

	lv_obj_remove_style_all(rcol);
	lv_obj_set_flex_grow(rcol, 1);
	lv_obj_set_height(rcol, lv_pct(100));
	lv_obj_set_flex_flow(rcol, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(rcol, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_remove_flag(rcol, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(rcol, LV_OBJ_FLAG_CLICKABLE);

	/* 第一行：天气文字 + 时钟（两端对齐，靠 space_between） */
	lv_obj_t *l1 = lv_obj_create(rcol);

	lv_obj_remove_style_all(l1);
	lv_obj_set_width(l1, lv_pct(100));
	lv_obj_set_height(l1, 18);
	lv_obj_set_flex_flow(l1, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(l1, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* 天气文字（多云/晴）*/
	w_type = lv_label_create(l1);
	lv_label_set_text(w_type, "");
	lv_label_set_long_mode(w_type, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(w_type, 1);

	/* 实时时钟。★ 原来放的是天气更新时间，换成真的在走的时钟：
	 * 既是实用信息，也顺手当"联网校时生效没有"的指示灯（灰=未校时）。 */
	w_clock = lv_label_create(l1);
	lv_label_set_text(w_clock, "--:--:--");
	lv_obj_set_style_text_color(w_clock, lv_color_hex(0x4A5A70), 0);

	/* 第二行：城市 / 湿度 / AQI，超长省略号截断（不再靠写死宽度） */
	w_detail = lv_label_create(rcol);
	lv_label_set_text(w_detail, "");
	lv_obj_set_style_text_color(w_detail, UI_C_DIM, 0);
	lv_obj_set_width(w_detail, lv_pct(100));
	lv_label_set_long_mode(w_detail, LV_LABEL_LONG_DOT);

	/* 轮询结果 + 定时刷新 */
	lv_timer_create(w_poll_cb, 200, NULL);
	/* ★ 初始周期用 RETRY_MS 而不是 REFRESH_MS：
	 *   自适应周期是在回调里改的，如果这里就写 10 分钟，
	 *   那"没数据→20 秒重试"要等 10 分钟后第一次触发才生效，等于没做。 */
	lv_timer_create(w_refresh_timer_cb, RETRY_MS, NULL);
	lv_timer_create(w_clock_cb, 1000, NULL);	/* 每秒走一格 */

	w_clock_update();	/* 先画一次，别让第一秒显示 --:--:-- */

	/* 立刻拉一次 */
	weather_fetch(DEFAULT_CITY_CODE);

	return card;
}
