/**
 * avatar.cpp —— 桌面形象：角色表和"记住选了哪个"
 *
 * 【配置存哪】
 *   /userdata/avatar.id —— 就一个数字。
 *   选 /userdata 是因为它是可写的用户分区（wpa_supplicant.conf 也在那儿），
 *   rootfs 是 ubifs 只读语义为主、空间只有几 MB，不适合放"用户设置"。
 *
 * 【读写失败不报错，只用默认值】
 *   形象是装饰性功能，文件坏了/没写权限不该影响开机。
 *   读失败 → 用 0 号角色；写失败 → 本次选择仍然生效（内存里），只是重启会丢。
 *   这一点和"驱动没装"那种硬错误不一样：那种要显式报给用户，这个不用。
 */
#include <cstdio>
#include <cstring>

#include "avatar.h"

#define AVATAR_CFG	"/userdata/avatar.id"

namespace {

/* 角色表。加角色 = 在这里加一行 + 改 count() 的返回值（用数组长度算，不用手改） */
const struct avatar_style kStyles[] = {
	{
		.name      = "橙猫",
		.desc      = "圆脸猫耳，最活泼",
		.ear       = AV_EAR_CAT,
		.face      = 0xF0A050,
		.ear_color = 0xF08040,
		.eye       = 0x2A1A0A,
		.cheek     = 0xFF8080,
		.mouth     = 0x5A2A10,
	},
	{
		.name      = "小蓝机",
		.desc      = "方脑袋机器人，冷静",
		.ear       = AV_EAR_ANTENNA,
		.face      = 0x6AA8E8,
		.ear_color = 0x3A78B8,
		.eye       = 0x101820,
		.cheek     = 0x8FC8FF,
		.mouth      = 0x2A4A70,
	},
	{
		.name      = "豆芽",
		.desc      = "两片叶子，绿油油",
		.ear       = AV_EAR_LEAF,
		.face       = 0x9AD86A,
		.ear_color = 0x5AA02A,
		.eye       = 0x1A3010,
		.cheek     = 0xD8F0A0,
		.mouth     = 0x3A5A20,
	},
	{
		.name      = "小幽灵",
		.desc      = "没有耳朵，白白的",
		.ear       = AV_EAR_NONE,
		.face      = 0xE8ECF4,
		.ear_color = 0xC0C8D8,
		.eye       = 0x304050,
		.cheek     = 0xFFC0C8,
		.mouth     = 0x506070,
	},
};

constexpr int kCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));

class AvatarStore {
public:
	int count() const { return kCount; }

	const struct avatar_style *style(int id) const
	{
		if (id < 0 || id >= kCount)
			return nullptr;
		return &kStyles[id];
	}

	int get() const { return id_; }

	int set(int id)
	{
		if (id < 0)
			id = 0;
		if (id >= kCount)
			id = kCount - 1;

		id_ = id;
		save(id);
		return id_;
	}

	/* 开机读一次。文件不存在或内容非法都退回 0，不报错 */
	void load()
	{
		FILE *f = std::fopen(AVATAR_CFG, "r");
		int v = 0;

		if (!f)
			return;

		if (std::fscanf(f, "%d", &v) == 1 && v >= 0 && v < kCount)
			id_ = v;

		std::fclose(f);
	}

private:
	void save(int id) const
	{
		FILE *f = std::fopen(AVATAR_CFG, "w");

		if (!f)
			return;		/* 写不进去就算了，内存里已经生效 */

		std::fprintf(f, "%d\n", id);
		std::fclose(f);
	}

	int id_ = 0;
};

AvatarStore g_store;

int api_count(void) { return g_store.count(); }
const struct avatar_style *api_style(int id) { return g_store.style(id); }
int api_get(void) { return g_store.get(); }
int api_set(int id) { return g_store.set(id); }

const struct avatar_ops kOps = {
	api_count,
	api_style,
	api_get,
	api_set,
};

} /* namespace */

extern "C" const struct avatar_ops *avatar_api(void)
{
	/* 第一次拿表的时候顺手把配置读进来 —— 省得 main 里再记一次调用 */
	static bool loaded = false;

	if (!loaded) {
		g_store.load();
		loaded = true;
	}

	return &kOps;
}
