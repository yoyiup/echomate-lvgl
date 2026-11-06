/*
 * ui_gallery.c — 浏览摄像头保存的照片与录像。
 *
 * 只扫描 CAM_DIR，不递归、不读文件内容：相机页是唯一写入入口，
 * 目录有限且文件名由相机模块产生。列表按文件名排序，显示格式、
 * 字节数和修改时间。视频点击显示所在路径，交由用户之后取回播放。
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "ui.h"

#define GALLERY_DIR "/tmp/cam"
#define GALLERY_MAX_ITEMS 64

struct gallery_item {
	char name[128];
	char type[8];
	unsigned long size;
	time_t mtime;
};

static lv_obj_t *page_gallery;
static lv_obj_t *gallery_list;
static lv_obj_t *gallery_status;
static struct gallery_item items[GALLERY_MAX_ITEMS];
static int item_count;

static int has_suffix(const char *name, const char *suffix)
{
	size_t n = strlen(name), s = strlen(suffix);
	return n >= s && strcasecmp(name + n - s, suffix) == 0;
}

static int item_compare(const void *a, const void *b)
{
	const struct gallery_item *ia = a, *ib = b;
	if (ia->mtime > ib->mtime) return -1;
	if (ia->mtime < ib->mtime) return 1;
	return strcmp(ib->name, ia->name);
}

static void show_item_cb(lv_event_t *e)
{
	int index = (int)(lv_intptr_t)lv_event_get_user_data(e);
	char path[192];
	if (index < 0 || index >= item_count)
		return;
	snprintf(path, sizeof(path), GALLERY_DIR "/%s", items[index].name);
	lv_label_set_text_fmt(gallery_status, "%s  %lu B\n%s",
			      items[index].type, items[index].size, path);
}

static void rebuild_gallery(void)
{
	DIR *dir = opendir(GALLERY_DIR);
	struct dirent *entry;
	item_count = 0;
	lv_obj_clean(gallery_list);

	if (!dir) {
		lv_label_set_text(gallery_status, "相册目录暂不可用");
		return;
	}

	while ((entry = readdir(dir)) != NULL && item_count < GALLERY_MAX_ITEMS) {
		struct stat st;
		struct gallery_item *item;
		char path[192];
		const char *type;

		if (entry->d_name[0] == '.')
			continue;
		if (has_suffix(entry->d_name, ".bmp")) type = "照片";
		else if (has_suffix(entry->d_name, ".avi")) type = "AVI";
		else if (has_suffix(entry->d_name, ".mp4")) type = "MP4";
		else continue;

		snprintf(path, sizeof(path), GALLERY_DIR "/%s", entry->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		item = &items[item_count++];
		snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
		snprintf(item->type, sizeof(item->type), "%s", type);
		item->size = (unsigned long)st.st_size;
		item->mtime = st.st_mtime;
	}
	closedir(dir);
	qsort(items, item_count, sizeof(items[0]), item_compare);

	if (item_count == 0) {
		lv_obj_t *empty = lv_label_create(gallery_list);
		lv_label_set_text(empty, "还没有照片或录像\n回到摄像头页拍摄");
		lv_obj_set_style_text_color(empty, UI_C_DIM, 0);
		lv_obj_set_style_pad_all(empty, 10, 0);
		lv_label_set_text(gallery_status, "相册为空");
		return;
	}

	for (int i = 0; i < item_count; ++i) {
		lv_obj_t *row = lv_button_create(gallery_list);
		lv_obj_t *label = lv_label_create(row);
		struct tm *tm_info = localtime(&items[i].mtime);
		char date[24] = "时间未知";
		if (tm_info)
			strftime(date, sizeof(date), "%m-%d %H:%M", tm_info);
		lv_obj_set_width(row, lv_pct(100));
		lv_obj_set_height(row, 38);
		lv_obj_set_style_bg_color(row, UI_C_LIST, 0);
		lv_obj_set_style_border_color(row, UI_C_LINE, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_pad_hor(row, 8, 0);
		lv_label_set_text_fmt(label, "%s  %s  %luB  %s", items[i].type,
				      items[i].name, items[i].size, date);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(label, lv_pct(100));
		lv_obj_set_style_text_color(label, UI_C_TEXT, 0);
		lv_obj_add_event_cb(row, show_item_cb, LV_EVENT_CLICKED,
				    (void *)(lv_intptr_t)i);
	}
	lv_label_set_text_fmt(gallery_status, "共 %d 个文件，点选查看路径", item_count);
}

static void refresh_cb(lv_event_t *e)
{
	(void)e;
	rebuild_gallery();
}

static lv_obj_t *build_gallery(void)
{
	lv_obj_t *scr = ui_make_screen("相册", true);
	lv_obj_t *body = ui_screen_body(scr);
	lv_obj_t *toolbar = lv_obj_create(body);
	lv_obj_t *refresh = lv_button_create(toolbar);
	lv_obj_t *refresh_label = lv_label_create(refresh);

	lv_obj_set_style_pad_all(body, 4, 0);
	lv_obj_set_style_pad_row(body, 4, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_set_size(toolbar, lv_pct(100), 38);
	lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(toolbar, 0, 0);
	lv_obj_set_style_pad_all(toolbar, 0, 0);
	lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_END,
		      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_size(refresh, 76, 32);
	lv_label_set_text(refresh_label, "刷新");
	lv_obj_center(refresh_label);
	lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, NULL);

	gallery_list = lv_obj_create(body);
	lv_obj_set_width(gallery_list, lv_pct(100));
	lv_obj_set_flex_grow(gallery_list, 1);
	lv_obj_set_style_bg_opa(gallery_list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(gallery_list, 0, 0);
	lv_obj_set_style_pad_all(gallery_list, 0, 0);
	lv_obj_set_style_pad_row(gallery_list, 3, 0);
	lv_obj_set_flex_flow(gallery_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(gallery_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(gallery_list, LV_SCROLLBAR_MODE_AUTO);

	gallery_status = lv_label_create(body);
	lv_obj_set_width(gallery_status, lv_pct(100));
	lv_obj_set_style_text_color(gallery_status, UI_C_DIM, 0);
	lv_label_set_long_mode(gallery_status, LV_LABEL_LONG_DOT);
	rebuild_gallery();
	return scr;
}

lv_obj_t *ui_page_gallery(void)
{
	if (!page_gallery)
		page_gallery = build_gallery();
	else
		rebuild_gallery();
	return page_gallery;
}
