/**
 * wifi_scan.h —— 真实 WiFi 扫描（后台线程）
 *
 * 【为什么必须异步】
 * 板上实测 `iw dev wlan0 scan` 一次要 **2.16 秒**。
 * LVGL 主循环是靠 lv_timer_handler() 以几十毫秒的节奏转的，
 * 在主循环里同步调 popen 会把界面冻住两秒多 —— 触摸不响应、不重绘，
 * 用户以为程序死了。
 *
 * 所以拆成两半，这正是 Linux 里"上半部/下半部"的思路：
 *   上半部：wifi_scan_start() 立刻返回，只在后台线程里干活
 *   下半部：UI 每隔 200ms 用 wifi_scan_copy() 看 seq 变没变，
 *           变了才重建列表。
 * 跟驱动里 request_threaded_irq 的分工完全一样 ——
 * 硬中断只做最少的事，耗时的活交给线程/工作队列。
 *
 * 【为什么用 pthread 而不是 fork】
 * 扫描结果要写回本进程的内存给 UI 读。fork 出来的子进程有独立地址空间，
 * 得靠 pipe 把数据搬回来（还要自己定义序列化格式）。
 * pthread 共享地址空间，加个互斥锁就够了。
 */
#ifndef __WIFI_SCAN_H__
#define __WIFI_SCAN_H__

/* C / C++ 混编保护：实现已改为 .cpp，函数是 extern "C" 的 */
#ifdef __cplusplus
extern "C" {
#endif

/* 最多显示多少个 AP。屏只有 240px 高，一行 40px，
 * 一屏能看 4 个多，16 个足够滚动浏览。 */
#define WIFI_MAX_AP	16
#define WIFI_SSID_LEN	33	/* SSID 最长 32 字节 + 结束符 */

struct wifi_ap {
	char	ssid[WIFI_SSID_LEN];
	int	freq;		/* MHz，2412 = 信道 1 */
	int	signal;		/* dBm，负值，-40 很强 / -90 很弱 */
	int	secure;		/* 1 = 有加密（RSN/WPA） */
};

struct wifi_result {
	struct wifi_ap	aps[WIFI_MAX_AP];
	int		count;		/* 有效 AP 个数 */
	int		scanning;	/* 1 = 后台正在扫 */
	int		seq;		/* 每完成一次扫描 +1，UI 靠它判断要不要重建 */
	char		err[64];	/* 出错时的说明，空串=没问题 */
};

/* wlan0 是否存在（驱动加载了没）。返回 0/1。 */
int wifi_available(void);

/* 启动一次后台扫描。已有扫描在跑时直接返回，不会叠加。
 * 这是"上半部"：不阻塞，立刻返回。 */
void wifi_scan_start(void);

/* 取一份结果快照（内部加锁拷贝）。返回当前 seq，
 * UI 记住上次的 seq，比出变化就说明有新结果。 */
int wifi_scan_copy(struct wifi_result *out);

/* 初始化互斥锁。必须在第一次调用上面任何函数之前调一次。 */
void wifi_scan_init(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __WIFI_SCAN_H__ */
