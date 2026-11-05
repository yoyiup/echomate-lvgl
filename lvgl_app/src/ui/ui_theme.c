/**
 * ui_theme.c —— 页面框架：颜色、公共控件、导航栈
 *
 * 这里放"所有页面都要用"的东西。页面自己只管往里填内容。
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ui.h"

/* ============================================================
 * 字体：中文字模 + 符号 fallback
 *
 * 【为什么需要这个】
 * 我们的字模 lv_font_cn_16 只覆盖三个区间：
 *   ASCII 0x20-0x7F、全角标点 0x3000-0x301F、汉字 0x4E00-0x9FA5
 * 而 LVGL 的内置符号（LV_SYMBOL_OK / BACKSPACE / CLOSE ...）
 * 全在 Unicode 私用区 0xF000-0xF8FF，一个都不在里面。
 *
 * 结果就是键盘上的回车、退格、确定、取消按钮全部空白 ——
 * 用户只能看到字母和符号键，功能键一片黑。
 *
 * 【解法：font fallback】
 * lv_font_t 有个 fallback 字段，lv_font_get_glyph_dsc() 在本字体
 * 找不到字形时会顺着这条链往下找（lv_font.c:76-90）。
 * 把 fallback 指向内置的 montserrat_14 —— 它的生成参数里
 * 就包含了这些符号（查看 lv_font_montserrat_14.c 头部注释：
 * 码点 61452=0xF00C OK、61453=0xF00D CLOSE、62810=0xF55A BACKSPACE）。
 *
 * 【为什么不用 const 直接改】
 * lv_font_cn_16 是 const 对象，在只读段，改它的字段会段错误。
 * 所以要拷一份结构体出来（≈60 字节），改副本的 fallback。
 * 字形数据本身不拷贝，仍然共享同一份，内存开销可以忽略。
 *
 * 【已知的小瑕疵】
 * fallback 的 line_height 是 14，主字体是 20。
 * LVGL 注释里说 fallback 最好同 line_height，否则符号看起来
 * 会比文字小一号。实测在键盘上不明显，先这样。
 * 真要完美就得用 lv_font_conv 单独生成一份 16px 的 FontAwesome
 * 字体（line_height 对齐到 20），那是后话。
 */
static lv_font_t g_font_ui;
static bool g_font_ready;

const lv_font_t *ui_font(void)
{
	if (!g_font_ready) {
		/* 结构体拷贝：g_font_ui = lv_font_cn_16 */
		g_font_ui = lv_font_cn_16;
		g_font_ui.fallback = &lv_font_montserrat_14;
		g_font_ready = true;
	}
	return &g_font_ui;
}

/**
 * 把一个对象树的字体换成带符号 fallback 的版本
 *
 * ★ 必须同时设两个 part，只设 MAIN 不够：
 *
 *   LV_PART_MAIN  普通文字（label、textarea、button 里的 label）
 *   LV_PART_ITEMS buttonmatrix / 键盘的按钮文字！
 *
 * lv_buttonmatrix.c:694 用的是
 *     lv_obj_init_draw_label_dsc(obj, LV_PART_ITEMS, &dsc)
 * 也就是键盘上每个键的文字读的是【ITEMS】部件的字体。
 *
 * 而 text_font 虽然在 LVGL 里标为可继承属性，但继承是
 * "父对象的 MAIN → 子对象的 MAIN"，同一对象的 MAIN 不会
 * 自动喂给自己的 ITEMS。所以只设 MAIN 时，键盘上
 * LV_SYMBOL_* 依然找不到字形（我第一版就是这么漏的，
 * 日志里 30 条 "glyph dsc. not found for U+F55A"）。
 */
void ui_apply_font(lv_obj_t *obj)
{
	if (!obj)
		return;

	lv_obj_set_style_text_font(obj, ui_font(), LV_PART_MAIN);
	lv_obj_set_style_text_font(obj, ui_font(), LV_PART_ITEMS);
}

/* 全局设置：多个页面共享的唯一数据源 */
struct ui_settings g_settings = {
	.wake_word	= true,
	.voice		= true,
	.auto_bright	= false,
	.camera		= true,
	.volume		= 60,
};

/* ============================================================
 * 导航栈
 *
 * 【为什么需要栈】
 * 返回时要回到"上一页"，而 LVGL 的 screen 之间没有父子关系，
 * 它不知道你是从哪一页跳过来的。所以要自己记。
 *
 * 【为什么不用固定几个变量】
 * 主页 → 网络 → 某个详情页，这种三层结构以后一定会有。
 * 栈是最省事的结构：进去 push，返回 pop，代码永远不会因为
 * 页面层数变化而改。
 * ============================================================ */
#define NAV_MAX 8

static lv_obj_t *nav_stack[NAV_MAX];
static int nav_top = -1;

/* 切页动画时长（ms）。
 *
 * ★ 关掉动画的原因（不是偷懒，是硬件算不过来）：
 *   MOVE_LEFT/MOVE_RIGHT 这类动画会把两个 screen 同时平移，
 *   每一帧的变化区域接近整屏。320x240x2 = 153600 字节/帧，
 *   200ms 要刷满 60fps 的话是 9MB/s。
 *   而这块屏走 SPI 60MHz，理论峰值 7.5MB/s，实测整屏刷新 28ms
 *   → 约 5.4MB/s，也就是最多 35fps。动画期间要求 60fps，
 *   必然掉帧、看起来一顿一顿。
 *
 *   在 240px 高的屏上，"滑入"带来的观感提升远不如一次
 *   干脆的瞬时切换。所以设 0 = 关动画。
 *   以后换了更快的屏（或者加了 DMA/双缓冲），把这里改回 200 即可。
 */
#define NAV_ANIM_MS 0

void ui_nav_init(lv_obj_t *root)
{
	nav_top = 0;
	nav_stack[0] = root;
}

void ui_nav_push(lv_obj_t *scr)
{
	if (!scr)
		return;

	/* 栈满了就不再压，但仍然把页面显示出来 —— 宁可返回时回不去，
	 * 也比点了没反应好。 */
	if (nav_top < NAV_MAX - 1)
		nav_stack[++nav_top] = scr;

	if (NAV_ANIM_MS > 0)
		lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT,
				    NAV_ANIM_MS, 0, false);
	else
		lv_screen_load(scr);
}

void ui_nav_back(void)
{
	if (nav_top <= 0)
		return;			/* 已经在栈底，没有上一页 */

	nav_top--;

	if (NAV_ANIM_MS > 0)
		lv_screen_load_anim(nav_stack[nav_top],
				    LV_SCR_LOAD_ANIM_MOVE_RIGHT,
				    NAV_ANIM_MS, 0, false);
	else
		lv_screen_load(nav_stack[nav_top]);
}

int ui_goto(const char *name)
{
	lv_obj_t *scr = NULL;

	if (!name)
		return -1;

	if (strcmp(name, "home") == 0)
		scr = ui_page_home();
	else if (strcmp(name, "net") == 0)
		scr = ui_page_net();
	else if (strcmp(name, "volume") == 0)
		scr = ui_page_volume();
	else if (strcmp(name, "about") == 0)
		scr = ui_page_about();
	else if (strcmp(name, "netcheck") == 0)
		scr = ui_page_netcheck();
	else if (strcmp(name, "camera") == 0)
		scr = ui_page_camera();
	else if (strcmp(name, "motor") == 0)
		scr = ui_page_motortest();
	else if (strcmp(name, "yolo") == 0)
		scr = ui_page_yolo();
	else if (strcmp(name, "aichat") == 0)
		scr = ui_page_aichat();
	else if (strcmp(name, "bright") == 0)
		scr = ui_page_brightness();
	else if (strcmp(name, "avatar") == 0)
		scr = ui_page_avatar();
	else
		return -1;

	/* 直接切过去，不带动画 —— 调试时想看的是页面本身的静态样子，
	 * 动画中间的过渡帧只会干扰截图。
	 *
	 * ★★ 但【必须压栈】。这里最早写的是 lv_screen_load(scr)，漏了压栈，
	 *    结果用 LVGL_START_PAGE=yolo 直接进页之后，点标题栏的「返回」
	 *    毫无反应 —— 因为 ui_nav_back() 第一行就是
	 *        if (nav_top <= 0) return;   // 已经在栈底
	 *    栈是空的，按钮等于装饰。表现成"卡在这个页面退不出去"，
	 *    而进程、画面都正常（UI 线程还在跑），极容易误判成死机。
	 *
	 *    正常从主页点进去走的是 goto_page_cb → ui_nav_push，所以没这个毛病；
	 *    只有调试直达这条路径会踩到。 */
	if (!scr)
		return 0;

	if (strcmp(name, "home") == 0) {
		/* 回主页就是回栈底，不要在栈上一直往上叠 */
		nav_top = 0;
		lv_screen_load(scr);
	} else {
		ui_nav_push(scr);
	}

	return 0;
}

/* ============================================================
 * 公共控件
 * ============================================================ */

/* 标题栏上的返回按钮 */
static void back_btn_cb(lv_event_t *e)
{
	(void)e;
	ui_nav_back();
}

/**
 * 建一个页面：screen + 标题栏 + 内容区
 *
 * 布局（flex 列）：
 *   screen
 *     ├── 标题栏 34px   [<]  标题
 *     └── 内容区 grow=1（吃掉剩下所有高度）
 *
 * 标题栏只给 34px 而不是主页原来的 40px：屏只有 240 高，
 * 每省 6px 就是列表多露出小半行。
 */
lv_obj_t *ui_make_screen(const char *title, bool with_back)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, UI_C_BG, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	/* 字体设在 screen 上，整页子对象靠继承生效 ——
	 * 这样新加控件不用记得单独设字体 */
	ui_apply_font(scr);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(scr, 0, 0);

	/* ---- 标题栏 ---- */
	lv_obj_t *bar = lv_obj_create(scr);
	lv_obj_set_width(bar, lv_pct(100));
	lv_obj_set_height(bar, 34);
	lv_obj_set_style_bg_color(bar, UI_C_BAR, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *t = lv_label_create(bar);
	lv_label_set_text(t, title);
	lv_obj_set_style_text_color(t, UI_C_TEXT, 0);
	/* 居中而不是靠左：有返回按钮时靠左会和按钮挤在一起 */
	lv_obj_center(t);

	if (with_back) {
		/* 返回按钮用中文「返回」而不是 "<"。
		 *
		 * 为什么：这个 16px 字模里的 ASCII 字形（如 '<'）实际只占
		 * 约 10px 高、且贴着基线偏上；而中文字形是满 16px 高。
		 * 两者混排时 '<' 看起来又小又偏顶，无论怎么 align 都调不齐
		 * ——那是字形本身的问题，不是布局问题。
		 * 换成中文后两个都满高，视觉自然对齐，语义也更直白。
		 *
		 * 宽度给 52px：两字各 16px + 左右留白。320 宽的栏里
		 * 标题仍然居中不受影响（标题是绝对居中的，不是跟着按钮排的）。
		 */
		lv_obj_t *btn = lv_obj_create(bar);
		lv_obj_set_size(btn, 52, 34);
		lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_radius(btn, 0, 0);
		lv_obj_set_style_pad_all(btn, 0, 0);
		lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *ar = lv_label_create(btn);
		lv_label_set_text(ar, "返回");
		lv_obj_set_style_text_color(ar, UI_C_ACCENT, 0);
		lv_obj_center(ar);
	}

	/* ---- 内容区 ---- */
	lv_obj_t *body = lv_obj_create(scr);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_flex_grow(body, 1);
	lv_obj_set_style_bg_color(body, UI_C_LIST, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_style_pad_row(body, 0, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	/* 把 body 挂到 screen 的 user_data 上。
	 * 不用 lv_obj_get_child(scr, -1) 取：那是"最后一个子对象"，
	 * 一旦以后往 screen 上再加个悬浮按钮，这个假设就破了。 */
	lv_obj_set_user_data(scr, body);

	return scr;
}

lv_obj_t *ui_screen_body(lv_obj_t *scr)
{
	return scr ? (lv_obj_t *)lv_obj_get_user_data(scr) : NULL;
}

/* 开关切换：把值写回全局设置 */
static void row_switch_cb(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target(e);
	bool *field = lv_event_get_user_data(e);
	bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

	/* 名字从同一行的第 0 个子对象（名字标签）读出来。
	 * 这依赖"ui_make_row 里 label 一定是第一个子对象"这个约定 ——
	 * 它是同一个函数里连续两行造出来的，顺序可控。
	 * 好处是不用额外传一个 label 参数，也不用维护 name 字段。 */
	lv_obj_t *row = lv_obj_get_parent(sw);
	lv_obj_t *lb = lv_obj_get_child(row, 0);
	const char *name = lb ? lv_label_get_text(lb) : "?";

	if (field)
		*field = on;

	printf("[开关] %s -> %s\n", name, on ? "开" : "关");
	ui_status("%s 已%s", name, on ? "打开" : "关闭");
}

/**
 * 造一个设置行
 *
 * 结构（flex 行）：
 *   [ 名字标签 grow=1 ]  [ 开关 ]  或  [ > ]
 *
 * lv_obj_set_flex_grow(label, 1) 是"左文右控件"布局的关键：
 * 让标签撑满剩余宽度，右边的开关自然被顶到最右侧。
 *
 * @param bind_field  非 NULL → 右侧放开关，初值取 *bind_field，
 *                    切换时把新值写回它。NULL → 右侧放 ">" 箭头。
 * @param cb          箭头模式下点击整行触发。
 */
lv_obj_t *ui_make_row(lv_obj_t *parent, const char *name, bool *bind_field,
		      lv_event_cb_t cb, void *user_data)
{
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
	/* 三参数：主轴 / 交叉轴 / 轨道。交叉轴 CENTER 让文字和开关垂直对齐。 */
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	/* ★ 这必须是第 0 个子对象：row_switch_cb 靠索引 0 取名字 */
	lv_obj_t *lb = lv_label_create(row);
	lv_label_set_text(lb, name);
	lv_obj_set_style_text_color(lb, UI_C_TEXT, 0);
	lv_obj_set_flex_grow(lb, 1);

	if (bind_field) {
		lv_obj_t *sw = lv_switch_create(row);
		lv_obj_set_size(sw, 42, 22);
		if (*bind_field)
			lv_obj_add_state(sw, LV_STATE_CHECKED);
		lv_obj_add_event_cb(sw, row_switch_cb, LV_EVENT_VALUE_CHANGED,
				    bind_field);
	} else {
		lv_obj_t *arrow = lv_label_create(row);
		lv_label_set_text(arrow, ">");
		lv_obj_set_style_text_color(arrow, UI_C_DIM, 0);

		if (cb)
			lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	}

	return row;
}

/* ============================================================
 * 底部状态条
 *
 * 这东西只在主页有，但子页也想报告操作结果（比如"已连接 XX"），
 * 所以做成全局单例：谁调 ui_status() 都写到主页那条上。
 * 用户返回主页时能看到刚才发生了什么。
 * ============================================================ */
static lv_obj_t *g_status_label;

void ui_status_bind(lv_obj_t *label)
{
	g_status_label = label;
}

void ui_status(const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	if (!g_status_label)
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* 状态条只有 28px 高、单行，长文本会被截断。
	 * 中文一个字 16px，320 宽的屏满打满算 18 个字，留点余量。 */
	lv_label_set_text(g_status_label, buf);
}
