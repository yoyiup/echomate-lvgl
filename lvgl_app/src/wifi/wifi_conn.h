/**
 * wifi_conn.h —— WiFi 连接管理（wpa_supplicant + udhcpc）
 *
 * 【为什么也必须异步】
 * 从"选网络"到"能上网"要经过四步，每一步都是秒级：
 *   关联 AP（WPA2 四次握手）  ~1-5 秒
 *   DHCP 拿 IP（discover/offer/request/ack） ~1-3 秒
 * 加起来可能 8 秒。在主循环里同步做，界面就是死 8 秒。
 * 所以和扫描一样：后台线程干，UI 靠状态轮询。
 *
 * 【为什么用 wpa_cli 而不是自己写配置文件】
 * 改配置文件的方式要重启 wpa_supplicant 才生效，而重启会把
 * 现有连接断掉。wpa_cli 通过控制接口（ctrl_interface）命令式操作，
 * 能"加一个网络并切过去"，不断其他连接，还能实时查状态。
 *
 * 完整命令序列（都在板上实测过）：
 *   wpa_cli -i wlan0 add_network                    → 返回网络 id
 *   wpa_cli -i wlan0 set_network <id> ssid <hex>    → hex 传，避开中文转义
 *   wpa_cli -i wlan0 set_network <id> psk "密码"
 *   wpa_cli -i wlan0 enable_network <id>
 *   wpa_cli -i wlan0 select_network <id>
 *   （轮询 status 直到 wpa_state=COMPLETED）
 *   udhcpc -i wlan0 -n -t 5 -T 3 -q
 */
#ifndef __WIFI_CONN_H__
#define __WIFI_CONN_H__

/* C / C++ 混编保护：实现已改为 .cpp，函数是 extern "C" 的 */
#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX	33
#define WIFI_PSK_MAX	64	/* 8~63 字符，留点余量 */

enum wifi_conn_state {
	WIFI_CONN_IDLE = 0,	/* 未连接 */
	WIFI_CONN_WORKING,	/* 正在连（关联/DHCP 都算） */
	WIFI_CONN_CONNECTED,	/* 已拿到 IP，可上网 */
	WIFI_CONN_FAILED,	/* 失败，看 err */
};

struct wifi_conn_info {
	int	state;
	char	ssid[WIFI_SSID_MAX];	/* 当前/目标 SSID */
	char	ip[16];			/* 拿到 IP 后填，如 192.168.2.139 */
	char	stage[24];		/* 细粒度阶段，给 UI 显示用 */
	char	err[72];
	int	seq;			/* 状态变化计数，UI 靠它判断要不要刷新 */
};

/* 初始化（建锁 + 清状态）。必须最先调一次。 */
void wifi_conn_init(void);

/* 异步连接。psk 传 NULL 或空串表示开放网络。
 * 已有连接在进行时直接返回，不会叠加。 */
void wifi_conn_start(const char *ssid, const char *psk);

/* 异步断开（保留 wpa_supplicant 进程，只是 disconnect + 释放 IP）。 */
void wifi_conn_disconnect(void);

/* 取状态快照，返回 seq。 */
int wifi_conn_get(struct wifi_conn_info *out);

/* 查某个 SSID 是不是已经在 wpa_supplicant 里配过（有密码记录）。
 * 有的话可以"免密直连"。返回 1/0。 */
int wifi_conn_is_saved(const char *ssid);

/* 立刻尝试用已保存的凭据连接（不弹密码框）。返回 1 表示已发起。 */
int wifi_conn_connect_saved(const char *ssid);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __WIFI_CONN_H__ */
