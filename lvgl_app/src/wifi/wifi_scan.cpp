/**
 * wifi_scan.c —— 调 `iw` 扫描，后台线程解析结果
 *
 * 【为什么调外部命令而不是直接调 nl80211】
 * 直接走 netlink 跟内核 cfg80211 对话是最"正宗"的方式，
 * 但要自己拼 nl80211 的消息结构、处理多播、解析嵌套属性 ——
 * 那是 libnl / iw 这一层该干的活，几百行起步。
 * `iw` 本来就在系统里（/usr/sbin/iw），它的输出格式稳定，
 * 用 popen 读文本是这里性价比最高的做法。
 *
 * 【iw 输出的两个坑】
 *
 * 1) 中文 SSID 被转义成 \xNN
 *    `SSID: \xe5\xb0\x8f\xe6\xa1\x83-2.4G` 其实是 UTF-8 编码的
 *    "小桃-2.4G"。iw 出于"输出可打印"的考虑，把非 ASCII 字节
 *    全部转义了。必须解码，否则屏上显示一长串 \xe5\xb0\x8f...
 *
 * 2) 一个 AP 的信息分散在多行
 *    以 "BSS xx:xx:..." 开头，后面跟着 freq / signal / SSID 等缩进行。
 *    所以要"见到 BSS 就开一条新记录，后面的行往里填"。
 *    隐藏 SSID（SSID 为空）的 AP 直接跳过 —— 显示空白行没意义。
 */
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <cstdbool>
#include <cctype>
#include <unistd.h>
#include <sys/stat.h>

#include <mutex>
#include <thread>

#include "wifi_scan.h"
#include "resources.hpp"     /* res::Pipe —— popen 的 RAII 封装 */

/* 全局结果 + 保护它的锁 */
static struct wifi_result g_res;
static std::mutex g_lock;

static const char *IW_PATH = "/usr/sbin/iw";
static const char *IFNAME  = "wlan0";

extern "C" void wifi_scan_init(void)
{
	memset(&g_res, 0, sizeof(g_res));
}

extern "C" int wifi_available(void)
{
	struct stat st;

	/* 判断网卡在不在，用 /sys/class/net/wlan0 这个目录最省事 ——
	 * 不用 ioctl、不用解析 ifconfig 输出。 */
	return stat("/sys/class/net/wlan0", &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * 把 iw 的 \xNN 转义还原成原始字节
 *
 * 输入形如：\xe5\xb0\x8f-2.4G
 * 输出："小-2.4G"（UTF-8 三字节 0xE5 0xB0 0x8F 就是"小"）
 *
 * 注意只在真的遇到 '\' 'x' 两个字符且后面是合法十六进制时才解码，
 * 普通反斜杠原样保留 —— SSID 里出现 '\' 是合法的。
 */
static void decode_ssid(const char *in, char *out, size_t outsz)
{
	size_t oi = 0;

	while (*in && oi + 1 < outsz) {
		if (in[0] == '\\' && in[1] == 'x' &&
		    isxdigit((unsigned char)in[2]) &&
		    isxdigit((unsigned char)in[3])) {
			char hex[3] = { in[2], in[3], 0 };
			out[oi++] = (char)strtol(hex, NULL, 16);
			in += 4;
		} else {
			out[oi++] = *in++;
		}
	}
	out[oi] = '\0';
}

/* 从 freq 换算信道号（只处理 2.4G，够用） */
static int freq_to_ch(int freq)
{
	if (freq == 2484)
		return 14;
	if (freq >= 2412 && freq <= 2472)
		return (freq - 2407) / 5;
	if (freq >= 5000)
		return (freq - 5000) / 5;
	return 0;
}

/* 信号强度换成"格数"：-50 以上 4 格，-80 以下 1 格 */
static int signal_bars(int dbm)
{
	if (dbm >= -50) return 4;
	if (dbm >= -65) return 3;
	if (dbm >= -80) return 2;
	return 1;
}

/* 按信号从强到弱排序（插入排序，最多 16 个，没必要上 qsort） */
static void sort_by_signal(struct wifi_result *r)
{
	int i, j;

	for (i = 1; i < r->count; i++) {
		struct wifi_ap key = r->aps[i];

		j = i - 1;
		while (j >= 0 && r->aps[j].signal < key.signal) {
			r->aps[j + 1] = r->aps[j];
			j--;
		}
		r->aps[j + 1] = key;
	}
}

/* 真正干活的部分：跑 iw、解析、写结果。在后台线程里执行。 */
static void do_scan(struct wifi_result *out)
{
	res::Pipe fp;
	char line[512];
	char cmd[128];
	int cur = -1;		/* 当前正在填的 AP 下标，-1 = 还没遇到 BSS */

	memset(out, 0, sizeof(*out));

	/* 拼命令行而不是写字面量：换网卡名/换 iw 路径只改上面两个常量 */
	snprintf(cmd, sizeof(cmd), "%s dev %s scan 2>/dev/null", IW_PATH, IFNAME);

	if (!fp.open(cmd)) {
		snprintf(out->err, sizeof(out->err), "popen 失败");
		return;
	}

	while (std::fgets(line, sizeof(line), fp.get())) {
		char *p = line;

		/* 去掉行首空白（iw 用 tab 缩进） */
		while (*p == ' ' || *p == '\t')
			p++;

		/* "BSS xx:xx:xx..." 开一条新记录 */
		if (strncmp(p, "BSS ", 4) == 0) {
			if (out->count >= WIFI_MAX_AP) {
				cur = -1;
				continue;
			}
			cur = out->count;
			memset(&out->aps[cur], 0, sizeof(out->aps[cur]));
			out->aps[cur].signal = -100;	/* 没读到 signal 时的兜底 */
			out->count++;
			continue;
		}

		if (cur < 0)
			continue;

		if (strncmp(p, "freq:", 5) == 0) {
			out->aps[cur].freq = atoi(p + 5);
		} else if (strncmp(p, "signal:", 7) == 0) {
			/* 形如 "signal: -58.00 dBm" */
			out->aps[cur].signal = (int)strtod(p + 7, NULL);
		} else if (strncmp(p, "SSID:", 5) == 0) {
			char raw[WIFI_SSID_LEN * 4];
			const char *s = p + 5;

			while (*s == ' ')
				s++;
			decode_ssid(s, raw, sizeof(raw));

			/* 去掉行尾换行 */
			size_t n = strlen(raw);

			while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r'))
				raw[--n] = '\0';

			/* 只保留前 32 字节，防止超长 SSID 撑爆界面 */
			snprintf(out->aps[cur].ssid, sizeof(out->aps[cur].ssid),
				 "%s", raw);
		} else if (strncmp(p, "RSN:", 4) == 0 ||
			   strncmp(p, "WPA:", 4) == 0) {
			out->aps[cur].secure = 1;
		}
	}

	fp.close();

	/* 清掉隐藏 SSID 的条目（SSID 为空）。
	 * iw 对隐藏网络不输出 SSID 行，那些条目留着只会显示空白行。 */
	{
		int i, w = 0;

		for (i = 0; i < out->count; i++) {
			if (out->aps[i].ssid[0] == '\0')
				continue;
			if (w != i)
				out->aps[w] = out->aps[i];
			w++;
		}
		out->count = w;
	}

	sort_by_signal(out);
}

/*
 * 扫描线程。
 *
 * ★ 这里原来用 pthread + detach。改成 std::thread 后同样是 detach
 *   （"启动就不管"的一次性任务）。参数本来就没有，所以不需要值捕获。
 */
static void scan_thread()
{
	struct wifi_result tmp;

	do_scan(&tmp);

	g_lock.lock();
	g_res.count = tmp.count;
	memcpy(g_res.aps, tmp.aps, sizeof(tmp.aps));
	snprintf(g_res.err, sizeof(g_res.err), "%s", tmp.err);
	g_res.scanning = 0;
	g_res.seq++;		/* ★ UI 靠这个值变化知道有新结果 */
	g_lock.unlock();
}

extern "C" void wifi_scan_start(void)
{
	int need = 0;

	g_lock.lock();
	if (!g_res.scanning) {
		g_res.scanning = 1;
		g_res.err[0] = '\0';
		need = 1;
	}
	g_lock.unlock();

	if (!need)
		return;		/* 已经在扫了，不重复启动 */

	/*
	 * detach：线程跑完自己回收，不用 join。
	 * 对"启动就不管"的一次性任务，这是最省事的方式。
	 * std::thread 构造失败会抛异常（板子内存紧张时真会发生）。
	 */
	try {
		std::thread th(scan_thread);

		th.detach();
	} catch (...) {
		g_lock.lock();
		g_res.scanning = 0;
		snprintf(g_res.err, sizeof(g_res.err), "创建扫描线程失败");
		g_lock.unlock();
	}
}

extern "C" int wifi_scan_copy(struct wifi_result *out)
{
	int seq;

	g_lock.lock();
	memcpy(out, &g_res, sizeof(*out));
	seq = g_res.seq;
	g_lock.unlock();

	return seq;
}
