/**
 * timesync.h —— 联网校时
 *
 * 【板子的时间问题】
 * RV1106 没有电池供电的 RTC，断电后内核时间从 0 开始（1970 或编译时刻）。
 * 系统自带的 /etc/init.d/S49ntp 会在开机早期拉起 ntpd -g，
 * 但那个时刻 WiFi 还没连上（WiFi 是 S99start_lvgl 里才装驱动、
 * lvgl_app 才去关联的），所以 ntpd 拿不到上游服务器，
 * 时间就一直停在上电时的值。
 *
 * 所以校时必须由「网络真的可用」这个事件来驱动，
 * 而不是由「开机」这个事件驱动。
 *
 * 【为什么自己实现 SNTP，不用 ntpd -q】
 * ntpd 无论作为客户端还是服务端都要 bind UDP 123，
 * 而系统的 ntpd 守护进程正占着这个端口，两者必然打架
 * （实测报 "unable to bind to wildcard address 0.0.0.0"）。
 * 要错开就得先停守护进程、校完再拉起来，多两个失败点。
 *
 * 标准 SNTP 客户端根本不需要绑定 123：用临时端口发一个 48 字节的
 * UDP 包，读回 48 字节，一次往返就够。既简单又不冲突。
 *
 * 【兜底】
 * 有些网络会封 UDP 123（企业网、公共 WiFi 常见）。
 * 那就退一步用 HTTP 响应头里的 Date 字段 —— 精度只有 1 秒，
 * 但显示时钟完全够用，而且和天气接口走同一条已验证可用的通路。
 */
#ifndef __TIMESYNC_H__
#define __TIMESYNC_H__

/* C / C++ 混编保护：实现已改为 .cpp，函数是 extern "C" 的 */
#ifdef __cplusplus
extern "C" {
#endif

/* 东八区。板子没有 /usr/share/zoneinfo，所以不能用 "Asia/Shanghai"，
 * 只能用 POSIX TZ 字符串：CST-8 = 中国标准时间，UTC+8，无夏令时。
 * （POSIX 的符号是反的：CST-8 表示"本地时间 = UTC + 8"）*/
#define TIMESYNC_TZ	"CST-8"

enum timesync_state {
	TIMESYNC_IDLE = 0,
	TIMESYNC_RUNNING,
	TIMESYNC_OK,
	TIMESYNC_FAILED,
};

struct timesync_info {
	int	state;
	long	offset_ms;	/* 校时前"系统时间 - 真实时间"，负=系统慢了 */
	char	source[32];	/* "ntp:ntp.aliyun.com" / "http:..." */
	char	when[16];	/* 校时完成后的本地时间 "00:44:22" */
	char	err[48];	/* 失败原因 */
	int	seq;		/* 状态变化计数，UI 靠它判断要不要刷新 */
};

/* 建锁。必须在任何 timesync_start() 之前调用。 */
void timesync_init(void);

/* 设时区（setenv TZ + tzset）。main() 一开始就该调，
 * 越早越好 —— 晚于第一次 localtime() 就没意义了。 */
void timesync_setup_tz(void);

/* 启动一次校时（异步线程，立刻返回）。
 *
 * 幂等：如果已经同步过、而且当前偏差小于 recheck 阈值，
 * 直接返回不动手 —— 网络抖动导致重连时不必反复校时。
 */
void timesync_start(void);

/* 取快照，返回 seq。 */
int timesync_get(struct timesync_info *out);

/* 当前偏差（秒，四舍五入）。未同步过返回 -1。
 * 用于界面判断"系统时间可不可信"。 */
int timesync_trusted(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __TIMESYNC_H__ */
