/**
 * avatar.h —— 桌面形象（可选角色的描述表 + 选择持久化）
 *
 * 【为什么形象不是图片】
 *   320x240 RGB565 的屏，放一张 88x88 的 PNG 要 ~15KB 解码后还要占内存，
 *   换角色就得带一堆图片资源。这里改成【用 LVGL 图形拼】：
 *   圆脸 = 圆角矩形，眼睛 = 小圆，嘴 = 圆角短条。
 *   好处是换颜色就换角色，动画（眨眼）只是改两个对象的高度，
 *   不占 flash、不占解码内存，也不需要图片素材。
 *   代价是只能做简单几何造型 —— 对"桌面小人"来说够了。
 *
 * 【数据放这里，绘制放 UI 层】
 *   颜色/造型参数属于"这个角色长什么样"，是数据；
 *   怎么摆到屏幕上属于 UI。分开之后，加角色只改这张表。
 */
#ifndef __AVATAR_H__
#define __AVATAR_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 耳朵/头饰造型 */
enum {
	AV_EAR_CAT = 0,		/* 两只圆猫耳 */
	AV_EAR_ANTENNA,		/* 一根天线 + 小球 */
	AV_EAR_LEAF,		/* 两片叶子（斜椭圆）*/
	AV_EAR_NONE,		/* 光头 */
};

struct avatar_style {
	const char	*name;		/* 中文名，显示在列表里 */
	const char	*desc;		/* 一句话描述 */
	int		ear;		/* AV_EAR_* */
	unsigned	face;		/* 脸的颜色 */
	unsigned	ear_color;	/* 耳朵/头饰 */
	unsigned	eye;		/* 眼睛 */
	unsigned	cheek;		/* 腮红 */
	unsigned	mouth;		/* 嘴 */
};

struct avatar_ops {
	/* 可选角色数量 */
	int  (*count)(void);

	/* 取第 id 个角色的造型；id 越界返回 NULL */
	const struct avatar_style *(*style)(int id);

	/* 当前选中（0 .. count-1）。读不到配置时默认 0 */
	int  (*get)(void);

	/* 选中并保存。返回实际生效的 id（越界会被夹到范围内）*/
	int  (*set)(int id);
};

/* 全模块唯一 C 入口 */
const struct avatar_ops *avatar_api(void);

#ifdef __cplusplus
}
#endif

#endif /* __AVATAR_H__ */
