/**
 * ui_wifidlg.c —— 密码输入对话框
 *
 * 240px 高的屏上做键盘，空间非常紧。布局只能这样切：
 *   顶部 32px   SSID（让用户确认连的是哪个网）
 *   中间 30px   密码输入框（星号显示）
 *   底部 168px  LVGL 键盘（占 70%）
 * 加起来 230，余 10px 边距。
 *
 * 【为什么挂在 lv_layer_top() 上】
 * layer_top 在所有 screen 之上，也不受页面切换影响。
 * 挂当前 screen 上的话，弹出时如果正好切页，对话框会跟着消失。
 * （和 lv_msgbox_create(NULL) 用的是同一种做法）
 *
 * 【键盘的三个必须设置】
 *   lv_keyboard_set_textarea()  —— 绑定输入框，不然按键没反应
 *   LV_EVENT_READY              —— 用户点右下角"确定"
 *   LV_EVENT_CANCEL             —— 用户点左上角"取消"
 * 两个事件都必须处理，否则对话框关不掉，会一直挡着。
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "../wifi/wifi_conn.h"

/* 对话框里的上下文 */
struct psk_dlg {
	lv_obj_t	*overlay;	/* 全屏遮罩（删它就全没了）*/
	lv_obj_t	*ta;		/* 输入框 */
	lv_obj_t	*hint;		/* 输入框下方的提示行（字数/错误） */
	char		ssid[WIFI_SSID_MAX];
	void		(*on_done)(const char *ssid, const char *psk);
};

static struct psk_dlg g_dlg;

static void dlg_close(void)
{
	if (g_dlg.overlay) {
		lv_obj_delete(g_dlg.overlay);
		g_dlg.overlay = NULL;
	}
	g_dlg.ta = NULL;
}

/* 关闭并回调。psk 为 NULL 表示用户取消。 */
static void dlg_finish(const char *psk)
{
	char ssid[WIFI_SSID_MAX];
	char psk_copy[WIFI_PSK_MAX + 1];
	void (*cb)(const char *, const char *) = g_dlg.on_done;

	snprintf(ssid, sizeof(ssid), "%s", g_dlg.ssid);

	/* ★★★ psk 必须【先拷贝】再关对话框。
	 *
	 * 进来的 psk 通常是 lv_textarea_get_text(g_dlg.ta) 的返回值，
	 * 指向 textarea 内部的文本缓冲。dlg_close() 会删除 overlay →
	 * 连带删除 textarea → 那块缓冲被 LVGL 回收。
	 * 之后再把 psk 传给回调就是 use-after-free：
	 * 轻则密码变成空串/乱码（wpa_cli 收到垃圾，连接必失败，
	 * 界面还只会报"密码错误"），重则踩到已回收内存直接崩。
	 *
	 * psk == NULL 是"取消"语义，要原样保留，不能用空串代替。 */
	if (psk) {
		snprintf(psk_copy, sizeof(psk_copy), "%s", psk);
		psk = psk_copy;
	}

	g_dlg.on_done = NULL;
	dlg_close();

	if (cb)
		cb(ssid, psk);
}

/* 输入框内容变化：更新字数提示
 *
 * 【为什么要有这行字】
 * 原来校验失败时调 ui_status() 写主页底部状态条 ——
 * 但对话框是全屏遮罩，把主页整个盖住了，用户根本看不到提示。
 * 等到关掉对话框才看到那句"密码至少 8 位"残留，
 * 于是以为"又失败了/密码不够长"，其实早就通过了。
 *
 * 提示必须显示在对话框内部，而且实时显示字数，
 * 让用户能自己判断"我到底输进去几位"，不用猜。 */
static void ta_changed_cb(lv_event_t *e)
{
	const char *txt = lv_textarea_get_text(lv_event_get_target(e));
	int n = (int)strlen(txt);

	if (!g_dlg.hint)
		return;

	if (n < 8) {
		lv_label_set_text_fmt(g_dlg.hint, "已输入 %d 位，还差 %d 位", n, 8 - n);
		lv_obj_set_style_text_color(g_dlg.hint, lv_color_hex(0xE8C060), 0);
	} else {
		lv_label_set_text_fmt(g_dlg.hint, "已输入 %d 位，可以点右下角 ✓ 提交", n);
		lv_obj_set_style_text_color(g_dlg.hint, lv_color_hex(0x5FD08A), 0);
	}
}

static void kb_event_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_READY) {
		const char *txt = lv_textarea_get_text(g_dlg.ta);

		printf("[WiFi] 用户输入密码长度=%d\n", (int)strlen(txt));

		/* WPA2 的 PSK 必须是 8~63 个字符。
		 * 在这里就拦住，别等 wpa 报错再回头查原因。 */
		if (strlen(txt) < 8) {
			/* 提示写在对话框里，不写主页状态条（被遮罩挡住了） */
			if (g_dlg.hint) {
				lv_label_set_text_fmt(g_dlg.hint,
						      "只有 %d 位，至少要 8 位",
						      (int)strlen(txt));
				lv_obj_set_style_text_color(g_dlg.hint,
							    lv_color_hex(0xD97070), 0);
			}
			return;		/* 不关对话框，让用户继续改 */
		}
		dlg_finish(txt);
	} else if (code == LV_EVENT_CANCEL) {
		printf("[WiFi] 用户取消输入密码\n");
		dlg_finish(NULL);
	}
}

/* 顶部的 × 关闭按钮 */
static void close_btn_cb(lv_event_t *e)
{
	(void)e;
	dlg_finish(NULL);
}

void ui_wifi_psk_dialog(const char *ssid,
			void (*on_done)(const char *ssid, const char *psk))
{
	lv_obj_t *kb, *ta, *lb;

	/* 已经开着就先关掉，避免叠两层 */
	dlg_close();

	memset(&g_dlg, 0, sizeof(g_dlg));
	snprintf(g_dlg.ssid, sizeof(g_dlg.ssid), "%s", ssid);
	g_dlg.on_done = on_done;

	/* ---- 全屏遮罩 ---- */
	lv_obj_t *ov = lv_obj_create(lv_layer_top());

	lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(ov, 0, 0);
	lv_obj_set_style_bg_color(ov, lv_color_hex(0x0A1628), 0);
	lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ov, 0, 0);
	lv_obj_set_style_radius(ov, 0, 0);
	lv_obj_set_style_pad_all(ov, 0, 0);
	lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
	/* 双保险：layer_top 上已经设过统一字体，但这里再显式设一次。
	 * 万一以后有人改了 layer_top 的设置，键盘不至于又变空白。 */
	ui_apply_font(ov);
	g_dlg.overlay = ov;

	/* ---- 顶部：SSID ---- */
	lb = lv_label_create(ov);
	lv_label_set_text_fmt(lb, "连接到 %s", ssid);
	lv_obj_set_style_text_color(lb, UI_C_TEXT, 0);
	lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 8, 8);
	/* 长 SSID 可能超出宽度，给它一个上限后自动省略 */
	lv_obj_set_width(lb, 260);
	lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

	/* 右上角关闭 */
	{
		lv_obj_t *cb = lv_obj_create(ov);

		lv_obj_set_size(cb, 28, 26);
		lv_obj_set_style_bg_opa(cb, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(cb, 0, 0);
		lv_obj_set_style_radius(cb, 0, 0);
		lv_obj_set_style_pad_all(cb, 0, 0);
		lv_obj_align(cb, LV_ALIGN_TOP_RIGHT, -4, 4);
		lv_obj_add_flag(cb, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(cb, close_btn_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *x = lv_label_create(cb);

		lv_label_set_text(x, "X");
		lv_obj_set_style_text_color(x, UI_C_DIM, 0);
		lv_obj_center(x);
	}

	/* ---- 中间：密码输入框 ----
	 * 高度分配（总共 240px，很紧）：
	 *   0   - 24   标题 + 关闭按钮
	 *   24  - 52   输入框
	 *   52  - 68   提示行（字数 / 错误）
	 *   72  - 240  键盘（168px）
	 */
	ta = lv_textarea_create(ov);
	lv_obj_set_size(ta, 300, 28);
	lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 24);
	lv_textarea_set_one_line(ta, true);		/* 单行，不要滚动条 */
	lv_textarea_set_password_mode(ta, true);	/* 显示成星号 */
	lv_textarea_set_placeholder_text(ta, "输入密码");
	lv_obj_set_style_bg_color(ta, lv_color_hex(0x1A2A4A), 0);
	lv_obj_set_style_text_color(ta, UI_C_TEXT, 0);
	lv_obj_set_style_border_color(ta, UI_C_ACCENT, 0);
	lv_obj_set_style_border_width(ta, 1, 0);
	lv_obj_set_style_pad_all(ta, 2, 0);
	g_dlg.ta = ta;

	/* 内容一变就更新字数，用户能直接看到"输进去几位" */
	lv_obj_add_event_cb(ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	/* ---- 提示行：字数 / 错误信息 ----
	 * ★ 必须放在对话框里，不能写主页状态条 ——
	 * 遮罩把主页全盖住了，写在那里用户看不到。 */
	g_dlg.hint = lv_label_create(ov);
	lv_label_set_text(g_dlg.hint, "已输入 0 位，还差 8 位");
	lv_obj_set_style_text_color(g_dlg.hint, lv_color_hex(0xE8C060), 0);
	lv_obj_align(g_dlg.hint, LV_ALIGN_TOP_LEFT, 12, 54);

	/* ---- 底部：键盘 ---- */
	kb = lv_keyboard_create(ov);
	lv_obj_set_size(kb, 320, 168);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_keyboard_set_textarea(kb, ta);

	/* ★★ 必须直接给键盘设字体，靠父对象继承的【无效】。
	 *
	 * 原因：LVGL 9 的默认主题（lv_theme_default）在
	 * lv_obj_class_init_obj() 时会按控件类型套一份默认样式，
	 * 其中就包含 text_font = LV_FONT_DEFAULT（我们的 cn_16，无 fallback）。
	 * 主题样式的优先级【高于】从父对象继承来的样式，
	 * 所以哪怕给 layer_top / overlay 都设了带 fallback 的字体，
	 * 键盘一创建就被主题的 cn_16 盖掉 →
	 * 日志里 30 多条 "glyph dsc. not found for U+F55A" 就是这么来的。
	 *
	 * lv_obj_set_style_xxx 是 local style，优先级最高，能压过主题。
	 * 所以必须"就地"设在键盘自己身上，不能指望继承。
	 */
	ui_apply_font(kb);

	lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

	/* ---- 键盘配色 ----
	 * 默认主题是浅色（白键黑字），在深色界面上非常跳。
	 * 统一成：深底 + 稍亮的键 + 浅色字。
	 *
	 * 注意每个 part 都要设：
	 *   MAIN   键盘整体背景
	 *   ITEMS  每个键的底（不设就是主题的白）
	 *   ITEMS|PRESSED  按下态，给一点反馈
	 */
	lv_obj_set_style_bg_color(kb, UI_C_BAR, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_pad_all(kb, 3, LV_PART_MAIN);
	lv_obj_set_style_pad_row(kb, 3, LV_PART_MAIN);
	lv_obj_set_style_pad_column(kb, 2, LV_PART_MAIN);

	lv_obj_set_style_bg_color(kb, lv_color_hex(0x2A3A5A), LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_text_color(kb, UI_C_TEXT, LV_PART_ITEMS);
	lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
	/* 键之间靠阴影/边框区分太费像素，用 1px 边就够 */
	lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);

	lv_obj_set_style_bg_color(kb, UI_C_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(kb, lv_color_hex(0x0A1628),
				    LV_PART_ITEMS | LV_STATE_PRESSED);

	/* 功能键（退格/回车/确定）在 LVGL 里叫 "checked" 态，
	 * 给它们换个颜色好和字母键区分 */
	lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A4A6A),
				  LV_PART_ITEMS | LV_STATE_CHECKED);

	printf("[WiFi] 弹出密码输入框: %s\n", ssid);
}

/**
 * 调试用：不连任何网络，单纯弹出键盘看布局
 *
 * 为什么需要：验证键盘字体/布局时，正常路径是"点某个 AP"，
 * 但那会触发真实 WiFi 连接，把网络状态改掉。
 * 这个函数只弹 UI，回调里什么都不做。
 */
static void demo_done(const char *ssid, const char *psk)
{
	printf("[WiFi] 调试键盘关闭: ssid=%s psk=%s\n",
	       ssid ? ssid : "-", psk ? "(已输入)" : "(取消)");
}

void ui_wifi_psk_dialog_demo(void)
{
	ui_wifi_psk_dialog("WPA2-测试网络-Test", demo_done);
}

/* 当前弹窗的目标 SSID（没弹窗时返回空串）。
 * 给自测用：验证"点击 AP 行 → 弹窗拿到的 SSID 是否正确"，
 * 也就是验证事件 user_data 有没有踩到悬垂指针。 */
const char *ui_wifi_dialog_current_ssid(void)
{
	return g_dlg.overlay ? g_dlg.ssid : "";
}

/* 强制关掉弹窗（不触发回调）。给自测清理现场用。 */
void ui_wifi_dialog_abort(void)
{
	g_dlg.on_done = NULL;
	dlg_close();
}
