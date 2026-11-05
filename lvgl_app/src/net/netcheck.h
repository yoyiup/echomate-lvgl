/**
 * netcheck.h —— 网络连通性自检
 *
 * 【为什么要做这个】
 * 上一轮踩过一个大坑：界面显示"已连接"、链路层 -48dBm/72Mbps 全正常，
 * 但 ping 什么都不通、还不报错（旧网段的 IP 残留在网卡上，
 * 网关在另一个网段，ARP 永远解析不出来）。
 *
 * 所以"连上了"这件事必须分层验证，每层用不同的判据：
 *   1. 关联     wpa_state=COMPLETED          —— 链路层握手成功
 *   2. IP       有 inet addr                  —— 拿到地址
 *   3. 网关     default 路由 + ARP 已解析     —— 能出本网段
 *   4. DNS      解析一个域名成功              —— 域名系统可用
 *   5. 外网     HTTP 拿到响应                 —— 真的通互联网
 *
 * 任意一层断掉，界面能直接指出是哪一层，不用再靠猜。
 *
 * 【为什么也做成异步】
 * 第 4、5 层每层都要几百毫秒到几秒（DNS 查询、TCP 三次握手 + HTTP）。
 * 五层串起来可能 3~5 秒，同步跑就会冻住 LVGL 主循环。
 */
#ifndef __NETCHECK_H__
#define __NETCHECK_H__

/* C / C++ 混编保护：实现已改为 .cpp，函数是 extern "C" 的 */
#ifdef __cplusplus
extern "C" {
#endif

/* 5 层网络 + 1 层时间。
 *
 * 为什么"时间"也算一层自检项：
 * 它和网络是一个耦合的整体 —— 板子没有电池 RTC，时间只能靠网络校准；
 * 而 TLS 证书校验、日志排序、天气数据的时效判断全都依赖正确的时间。
 * 所以"时间准不准"其实是"网络栈好不好用"的一部分，
 * 放在同一张表里比藏在关于页更有用。 */
#define NETCHECK_STEPS	6

enum netcheck_state {
	NETCHECK_IDLE = 0,
	NETCHECK_RUNNING,
	NETCHECK_DONE,
};

struct netcheck_step {
	char	name[16];	/* 层次名，如 "网关" */
	int	ok;		/* 1 = 通过, 0 = 失败, -1 = 未测 */
	char	detail[48];	/* 细节，如 "10.141.62.128" */
	int	ms;		/* 这一步耗时 */
};

struct netcheck_result {
	int			state;
	int			done;		/* 已完成几步 */
	struct netcheck_step	steps[NETCHECK_STEPS];
	char			summary[48];	/* 一句话结论 */
	int			seq;		/* 完成度变化计数 */
};

void netcheck_init(void);

/* 启动一次自检（异步）。已在跑时直接返回。 */
void netcheck_start(void);

/* 取快照，返回 seq。UI 靠 seq 变化判断要不要刷新。 */
int netcheck_get(struct netcheck_result *out);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __NETCHECK_H__ */
