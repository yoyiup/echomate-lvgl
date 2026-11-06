/**
 * ui_home.c —— 主页：设置列表
 *
 * 8 个条目，其中 3 个能跳转到子页（网络 / 音量 / 固件版本），
 * 1 个弹出确认对话框（恢复出厂设置），其余 4 个是开关。
 *
 * 【页面要点】
 * 列表故意做成 9 行高（360px）超过可视区，用来验证垂直滚动。
 * 滚动的关键是 body 上设 scroll_dir 和 scrollbar_mode，
 * 见 ui_make_screen 里 body 的创建。
 */
#include <stdio.h>

#include "ui.h"

static lv_obj_t *page_home;	/* 缓存，避免重复创建 */

/* ---------- 跳转回调 ----------
 * 三个子页的跳转逻辑完全一样，只是目标不同。
 * 所以用同一份回调 + 不同的 user_data 指向目标页的构建函数，
 * 而不是写三个几乎一样的函数。
 */
typedef lv_obj_t *(*page_builder_t)(void);

static void goto_page_cb(lv_event_t *e)
{
	page_builder_t build = lv_event_get_user_data(e);
	lv_obj_t *scr = build();

	if (scr)
		ui_nav_push(scr);
}

/* ---------- 恢复出厂设置：确认对话框 ---------- */

static void reset_confirm_cb(lv_event_t *e)
{
	lv_obj_t *mbox = lv_event_get_user_data(e);

	printf("[重置] 用户确认恢复出厂设置\n");
	g_settings.wake_word = true;
	g_settings.voice = true;
	g_settings.auto_bright = false;
	g_settings.camera = true;
	g_settings.volume = 60;

	ui_status("已恢复出厂设置");

	/* lv_msgbox_close 会把整个 dialog 连同背景遮罩一起删掉。
	 * 不删的话它会一直盖在屏上，用户点哪都没反应。 */
	lv_msgbox_close(mbox);
}

static void reset_cancel_cb(lv_event_t *e)
{
	lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

static void do_reset_cb(lv_event_t *e)
{
	(void)e;

	/* parent 传 NULL → 自动创建全屏半透明遮罩，挡住下面的列表。
	 * 防止用户在对话框上滑列表，这是模态对话框的意义。 */
	lv_obj_t *mbox = lv_msgbox_create(NULL);

	lv_msgbox_add_title(mbox, "恢复出厂设置");
	lv_msgbox_add_text(mbox, "所有设置将恢复默认值，\n此操作不可撤销。");

	lv_obj_t *btn_ok = lv_msgbox_add_footer_button(mbox, "确认重置");
	lv_obj_t *btn_no = lv_msgbox_add_footer_button(mbox, "取消");

	/* 注意 user_data 传的是谁：确认按钮要拿到 mbox 才能关掉自己。
	 * 回调里拿 lv_event_get_user_data 而不是从控件往上找父对象，
	 * 是因为 msgbox 内部层级比较深，向上遍历容易找错层。 */
	lv_obj_add_event_cb(btn_ok, reset_confirm_cb, LV_EVENT_CLICKED, mbox);
	lv_obj_add_event_cb(btn_no, reset_cancel_cb, LV_EVENT_CLICKED, mbox);
}

/* ---------- 构建主页 ---------- */

static lv_obj_t *build_home(void)
{
	lv_obj_t *scr = ui_make_screen("回声伙伴", false);
	lv_obj_t *body = ui_screen_body(scr);

	/* body 是 flex COLUMN 流（ui_make_screen 里设好的），
	 * 直接往里按顺序放两个孩子：
	 *   ① 天气卡片  固定高度 46
	 *   ② 设置列表  flex_grow(1) 吃掉剩下的全部高度
	 *
	 * 这样布局由 flex 自动算，不用手动"body 高 - 卡片高"
	 * （那样要读运行时高度，构建阶段读到的是 0，很容易算错）。 */

	/* ---- ① hero：桌面形象 + 天气卡片 ----
	 *
	 * 原来这里只有一张通栏天气卡。改成一行两块：
	 *   左边 72x72 的形象（可换角色），右边天气卡 flex_grow 吃掉剩余宽度。
	 * 为什么把头像放这儿而不是单独开一页：桌面形象的意义就是"待机时看得见"，
	 * 藏在子页里就失去意义了。
	 */
	lv_obj_t *hero = lv_obj_create(body);

	lv_obj_set_size(hero, lv_pct(100), 76);
	lv_obj_set_style_bg_color(hero, lv_color_hex(0x1E3050), 0);
	lv_obj_set_style_border_width(hero, 0, 0);
	lv_obj_set_style_radius(hero, 0, 0);
	lv_obj_set_style_pad_all(hero, 2, 0);
	lv_obj_set_style_pad_column(hero, 4, 0);
	lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* 头像本身不带背景。点它是"摸一下有反应"（眯眼+张嘴+蹦），
	 * 不再顺便跳转 —— 两个动作绑在一个点击上，反馈会被跳转吃掉，
	 * 人根本看不到。换角色走下面列表里的「桌面形象」入口。 */
	ui_avatar_create(hero, 72);

	/* 右侧：天气卡放在一个纵向容器里，卡片自己设 width 100% 就是这块的宽度 */
	lv_obj_t *wcol = lv_obj_create(hero);

	lv_obj_set_flex_grow(wcol, 1);
	lv_obj_set_height(wcol, lv_pct(100));
	lv_obj_set_style_bg_opa(wcol, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(wcol, 0, 0);
	lv_obj_set_style_pad_all(wcol, 0, 0);
	lv_obj_remove_flag(wcol, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(wcol, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(wcol, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	ui_weather_card(wcol);

	/* ---- ② 设置列表（可滚动）---- */
	lv_obj_t *body_list = lv_obj_create(body);

	lv_obj_set_width(body_list, lv_pct(100));
	lv_obj_set_flex_grow(body_list, 1);	/* 吃掉卡片之外的高度 */
	lv_obj_set_style_bg_color(body_list, UI_C_LIST, 0);
	lv_obj_set_style_border_width(body_list, 0, 0);
	lv_obj_set_style_radius(body_list, 0, 0);
	lv_obj_set_style_pad_all(body_list, 0, 0);
	lv_obj_set_style_pad_row(body_list, 0, 0);
	/* ★★★ 这里原来有一行 lv_obj_remove_flag(body_list, LV_OBJ_FLAG_SCROLLABLE)。
	 *
	 * 那是个 bug，代价是"整个主页不能上下滑动，但点击正常"。
	 *
	 * 原因：LV_OBJ_FLAG_SCROLLABLE 是"这个对象能不能滚"的【总开关】。
	 * 去掉它之后，下面那句 lv_obj_set_scroll_dir() 完全无效 ——
	 * 设方向只是"允许朝哪边滚"，前提是"能滚"。开关关了，
	 * 方向设成什么都是白设。
	 *
	 * 为什么症状是"能点不能滑"：点击只需要 PRESSED/RELEASED 事件，
	 * 而滑动需要对象真的滚起来。所以点击一切正常，滑动毫无反应，
	 * 而且控制台不会报任何错 —— 最容易让人怀疑是触摸坏了。
	 *
	 * 当时写这一行的心是"容器内部不要自己滚"，
	 * 但列表本来就是靠它自己滚的。真正不该滚的是更外层的 body。 */
	lv_obj_set_flex_flow(body_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body_list, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(body_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body_list, LV_SCROLLBAR_MODE_OFF);

	/* 外层 body 关掉滚动：它里面只有"天气卡片 + 列表"两个定长孩子，
	 * 内容不会超出自身高度，本来也滚不动。
	 * 关掉的好处是滚动责任唯一 —— 免得列表滚到底时
	 * 把滚动链冒泡给 body，出现"两个容器抢滚动"的诡异手感。
	 *
	 * ★ 注意不能无条件改：网络页/音量页/关于页都是把行
	 *   直接挂在 body 上、靠 body 滚动的。所以这一句只能写在
	 *   主页自己的构建函数里，不能挪进 ui_make_screen()。 */
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	/* 从这行往下，所有 ui_make_row 的父对象都是 body_list */

	/* 开关类：bind_field 指向全局设置里的字段，切换时自动写回 */
	ui_make_row(body_list, "唤醒词",       &g_settings.wake_word,   NULL, NULL);
	ui_make_row(body_list, "语音播报",     &g_settings.voice,       NULL, NULL);
	/* 亮度：原来是「自动调节亮度」开关，但那个开关只是改了个内存标志，
	 * 根本没人读它 —— 板子上其实有标准的背光设备
	 * （/sys/class/backlight/backlight，0~99），只是界面没接。
	 * 现在换成真正能调的入口，行为所见即所得。 */
	ui_make_row(body_list, "屏幕亮度", NULL, goto_page_cb, (void *)ui_page_brightness);
	ui_make_row(body_list, "桌面形象", NULL, goto_page_cb, (void *)ui_page_avatar);

	/* 跳转类：bind_field=NULL 表示右侧画 ">" */
	ui_make_row(body_list, "网络连接", NULL, goto_page_cb, (void *)ui_page_net);
	ui_make_row(body_list, "网络自检", NULL, goto_page_cb, (void *)ui_page_netcheck);
	ui_make_row(body_list, "音量",     NULL, goto_page_cb, (void *)ui_page_volume);

	/* 摄像头：点进去是实时预览 + 拍照 + 录像页面。
	 * 原来这里挂的是 g_settings.camera 开关（只有"开/关"没有界面），
	 * 现在改成跳转入口 —— 和网络/音量那几行同一种用法。 */
	ui_make_row(body_list, "摄像头", NULL, goto_page_cb, (void *)ui_page_camera);
	ui_make_row(body_list, "相册", NULL, goto_page_cb, (void *)ui_page_gallery);

	/* 电机测试：手动点按验证 /dev/echo_motor（两路 H 桥）。
	 * 属于"硬件调试入口"，平时不会用，排查接线时最有用。 */
	ui_make_row(body_list, "电机测试", NULL, goto_page_cb, (void *)ui_page_motortest);

	/* YOLO 相机：走 NPU 做实时目标检测，检测框直接叠在预览画面上。
	 * 第一次点进去要加载 7.6MB 模型，会顿一下（约 1 秒）。 */
	ui_make_row(body_list, "YOLO 相机", NULL, goto_page_cb, (void *)ui_page_yolo);

	/* AI 对话：语音链路做成"入口"，进页面才拉起（否则开机就占住麦克风） */
	ui_make_row(body_list, "AI 对话", NULL, goto_page_cb, (void *)ui_page_aichat);

	ui_make_row(body_list, "固件版本",     NULL, goto_page_cb,  (void *)ui_page_about);
	ui_make_row(body_list, "恢复出厂设置", NULL, do_reset_cb,   NULL);

	/* ---- 底部状态条 ----
	 * ★ 必须让它参与 screen 的 flex 布局，不能用绝对定位。
	 *
	 *   一开始我用的 lv_obj_align(footer, LV_ALIGN_BOTTOM_MID) 贴在底边，
	 *   结果 body 靠 flex_grow 撑满了整个剩余高度（206px），
	 *   状态条是【浮在上面】的 —— 列表滚到底时最后一行会被它盖掉半截。
	 *
	 *   正确做法：footer 作为 flex 列的第三个孩子（screen 是 COLUMN 流），
	 *   高度写死 28px，body 的 grow 自然只能拿到 240-34-28=178px。
	 *   这样滚动区域的下边界恰好是状态条的上边界，永不重叠。
	 */
	lv_obj_t *footer = lv_obj_create(scr);
	lv_obj_set_width(footer, lv_pct(100));
	lv_obj_set_height(footer, 28);
	lv_obj_set_style_bg_color(footer, UI_C_BAR, 0);
	lv_obj_set_style_border_width(footer, 0, 0);
	lv_obj_set_style_radius(footer, 0, 0);
	lv_obj_set_style_pad_hor(footer, 10, 0);
	lv_obj_set_style_pad_ver(footer, 0, 0);
	lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *st = lv_label_create(footer);
	lv_label_set_text(st, "上下滑动列表");
	lv_obj_set_style_text_color(st, UI_C_DIM, 0);

	/* 登记成全局状态条，子页也能往这里写 */
	ui_status_bind(st);

	return scr;
}

lv_obj_t *ui_page_home(void)
{
	if (!page_home)
		page_home = build_home();
	return page_home;
}
