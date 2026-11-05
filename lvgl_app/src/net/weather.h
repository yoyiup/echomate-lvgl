/**
 * weather.h —— 从 HTTP 接口取天气
 *
 * 【为什么用 t.weather.itboy.net 这个接口】
 * 板上 wget **不支持 HTTPS**（实测报 "not an http or ftp url"），
 * 而主流天气 API（和风、心知、OpenWeather）全部强制 HTTPS。
 * 这个接口是目前实测还提供明文 HTTP 的：
 *   http://t.weather.itboy.net/api/weather/city/<citycode>
 * 返回 JSON，含当前温度/湿度/天气/空气质量 + 未来几天预报。
 *
 * 广州的 citycode 是 101280101（和风天气的城市编码体系）。
 *
 * 【为什么解析要手写】
 * 板子上没有 JSON 库，为一个只有 4KB 的固定格式响应引入 cJSON
 * 不划算。这里用"找 key 再取值"的方式提取字段 ——
 * 关键在于每个 key 都带引号，形如 "wendu":"30.6"，
 * 所以搜索 `"wendu":"` 就能精确定位，不会误匹配到同名子串。
 */
#ifndef __WEATHER_H__
#define __WEATHER_H__

/*
 * ★ C / C++ 混编保护。
 *
 *   实现已经从 weather.c 改成 weather.cpp，里面的函数是 extern "C" 的。
 *   头文件如果按 C++ 声明（默认），调用方会按修饰名去找符号，链不上。
 *   纯 C 文件 include 本头时这段被 #ifdef 屏蔽，零影响。
 */
#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_CITY_MAX	24
#define WEATHER_TYPE_MAX	16
#define WEATHER_NOTICE_MAX	48

struct weather_info {
	int	temp;			/* 当前温度（整数部分）*/
	int	humidity;		/* 湿度 % */
	char	city[WEATHER_CITY_MAX];
	char	type[WEATHER_TYPE_MAX];	/* 天气：多云 / 晴 / 小雨 */
	char	notice[WEATHER_NOTICE_MAX];	/* 一句话提示 */
	int	aqi;			/* PM2.5，-1 表示没取到 */
	char	quality[8];		/* 优 / 良 / 轻度污染 */
	int	high;			/* 今日最高温 */
	int	low;			/* 今日最低温 */
	char	update_time[24];	/* 数据时间（"11-01 08:25"，给人看） */
	long	update_epoch;		/* 同上，转成 epoch 秒，给程序判断新旧用
					 * 0 = 没解析到（接口格式变了）
					 *
					 * ★ 为什么要存 epoch：
					 *   光有 "11-01 08:25" 这种字符串，UI 侧想判断
					 *   "这数据是不是太旧了"还得自己解析+补年份+算时区，
					 *   容易错。存成 epoch 后一句 `time(NULL) - ep` 就够。
					 *
					 * ★ 时区：timesync_setup_tz() 已经 setenv("TZ","CST-8")
					 *   + tzset()，所以 mktime 按东八区解释，和接口给的
					 *   北京时间天然对齐，**不需要手动 +8 小时**。
					 *   （板子的 `date` 命令显示 UTC，是因为它没继承那个
					 *     setenv —— setenv 只改进程自己的环境副本，
					 *     /proc/PID/environ 里看不到。这点很容易被误导。）*/
	bool	valid;			/* 取到有效数据 */
	char	err[48];
};

void weather_init(void);

/* 异步拉取。已在拉时直接返回。 */
void weather_fetch(const char *citycode);

/* 取快照，返回 seq。 */
int weather_get(struct weather_info *out);

/* 是否有请求正在进行 */
bool weather_busy(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __WEATHER_H__ */
