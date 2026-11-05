/**
 * timesync.c —— 联网校时（SNTP 客户端 + HTTP Date 兜底）
 *
 * 设计要点见 timesync.h 的注释。这里补三件事：
 *
 * 1) 为什么校时用「网络就绪」而不是「开机」触发
 *    开机时 WiFi 还没连上，任何依赖网络的动作都必然失败。
 *    所以由 wifi_conn 在拿到 IP + 路由 + ARP 都验证通过之后调用。
 *
 * 2) 为什么 settimeofday 不会把界面弄坏
 *    LVGL 的 tick 走 CLOCK_MONOTONIC（main.c 的 tick_poll），
 *    单调时钟不受 settimeofday 影响。如果哪天把 tick 改成
 *    gettimeofday，校时那一刻 lv_tick_inc 会收到一个巨大的 delta，
 *    界面会直接卡死或跳帧 —— 这是个必须记住的约束。
 *
 * 3) NTP 时间戳的纪元问题
 *    NTP 从 1900-01-01 起算，Unix 从 1970-01-01 起算，差 2208988800 秒。
 *    字段只有 32 位，2036-02-07 之后会回绕。到时 secs 会比这个差值还小，
 *    减法要用 64 位做，回绕后再加 2^32。现在（2026）还没到，但代码里
 *    留了处理，免得以后莫名其妙差 136 年。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mutex>
#include <thread>

#include "timesync.h"
#include "resources.hpp"	/* res::Fd / res::AddrInfo / res::Pipe */

#define NTP_PORT	123
#define NTP_PKT		48
/* 1900-01-01 → 1970-01-01 的秒数 */
#define NTP_EPOCH_DELTA	2208988800LL

/* 国内可达的 NTP 服务器。按响应速度排序，
 * 阿里云在广州节点通常 <20ms，腾讯次之，pool 兜底。 */
static const char *g_ntp_srv[] = {
	"ntp.aliyun.com",
	"ntp.tencent.com",
	"cn.pool.ntp.org",
};

/* HTTP Date 兜底用的地址。选这个是因为已经验证过
 * 它提供明文 HTTP（这个 busybox wget 不支持 https），
 * 而且就是天气卡片用的同一个接口，通路一定通。 */
#define HTTP_HOST	"t.weather.itboy.net"
#define HTTP_PATH	"/api/weather/city/101280101"

static struct timesync_info g_ts;
static std::mutex g_lock;
static bool g_busy;
/* 上次校时【成功之后经过了多少秒】的计时基准。
 *
 * ★ 这里踩过一个坑：原来存的是"校时成功那一刻的 time_t"，
 *   然后用 `now - g_last_ok < 300` 判断幂等窗口。
 *   但 settimeofday 之后系统时间会跳变，两者不在同一条时间线上：
 *   校时把时间往回拨 N 秒，now 就会比 g_last_ok 还小，
 *   减法变成负数，条件恒成立，幂等窗口失效 ——
 *   实测后果是自检页和 WiFi 连接各触发一次，连着校了两遍。
 *
 *   所以改用 CLOCK_MONOTONIC 当基准：它不受 settimeofday 影响，
 *   永远单调递增。这也是本项目 LVGL tick 用的时钟，理由相同。 */
static long long g_last_ok_mono_ms;

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* 时区                                                                */
/* ------------------------------------------------------------------ */

extern "C" void timesync_setup_tz(void)
{
	const char *old = getenv("TZ");

	setenv("TZ", TIMESYNC_TZ, 1);
	tzset();

	/* 打印一行证明生效了。板子上没有 zoneinfo，
	 * 所以只能靠 POSIX TZ 字符串，这里顺手验证一下：
	 * 如果 TZ 没生效，local 会和 UTC 完全一样。 */
	{
		time_t now = time(NULL);
		struct tm lt, gt;
		char lb[32], gb[32];

		localtime_r(&now, &lt);
		gmtime_r(&now, &gt);
		strftime(lb, sizeof(lb), "%m-%d %H:%M:%S", &lt);
		strftime(gb, sizeof(gb), "%m-%d %H:%M:%S", &gt);
		printf("[时区] TZ=%s（原 %s） 本地 %s / UTC %s\n",
		       TIMESYNC_TZ, old ? old : "未设置", lb, gb);
	}
}

/* ------------------------------------------------------------------ */
/* SNTP 客户端                                                         */
/* ------------------------------------------------------------------ */

/**
 * 向 host 查一次时间
 *
 * @param server_ms  出参：服务器当前时刻（Unix 毫秒）
 * @param offset_ms  出参：服务器 - 本地（正=本地慢了）
 * @return true 成功
 *
 * 报文只有一个字段有意义：首字节的 Mode。
 *   bit 0-2  Mode：3 = client, 4 = server
 *   bit 3-5  Version：3
 *   bit 6-7  Leap Indicator：0 = 正常
 * 所以客户端首字节 = 0b00_011_011 = 0x1B。
 *
 * 响应里取 bytes 40-47 的 Transmit Timestamp：
 *   高 32 位 = 自 1900 起的秒数，低 32 位 = 秒的小数部分。
 */
static bool sntp_query(const char *host, int timeout_ms,
		       long long *server_ms, long *offset_ms)
{
	struct timeval t0, t1, tv;
	unsigned char pkt[NTP_PKT] = {0};
	unsigned char rsp[NTP_PKT];
	long long l0, l1, local_mid, srv, frac;
	uint32_t secs, fr;
	bool ok = false;
	ssize_t n;

	/*
	 * ★ 两处资源改成 RAII：
	 *     getaddrinfo 的 res  → res::AddrInfo（析构自动 freeaddrinfo）
	 *     socket 的 fd        → res::Fd（析构自动 close）
	 *   原来它们靠 goto out + 末尾 if(fd>=0) close + freeaddrinfo 配对，
	 *   一共 6 处 goto 汇聚到同一个出口 —— 虽然写对了，但只要有人在
	 *   中间插一条新的 return，就会漏掉其中一样。
	 *   这个函数在断网时每 60 秒跑一次、每轮试最多 3 台服务器，
	 *   漏得很快。现在任何路径返回都会自动释放。
	 */
	res::AddrInfo res;
	res::Fd fd;

	/* AF_INET：板子只有 IPv4，不必试 v6 白等 */
	if (!res.resolve(host, "123", AF_INET, SOCK_DGRAM))
		return false;

	fd.reset(::socket(res.get()->ai_family, res.get()->ai_socktype, 0));
	if (!fd.valid())
		goto out;

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	pkt[0] = 0x1B;

	gettimeofday(&t0, NULL);
	if (sendto(fd.get(), pkt, sizeof(pkt), 0,
		   res.get()->ai_addr, res.get()->ai_addrlen) !=
	    (ssize_t)sizeof(pkt))
		goto out;

	n = recv(fd.get(), rsp, sizeof(rsp), 0);
	gettimeofday(&t1, NULL);
	if (n != (ssize_t)sizeof(rsp))
		goto out;

	/* Mode 必须回 4（server），否则可能是别的服务的乱包 */
	if ((rsp[0] & 0x07) != 4)
		goto out;

	/* 0 = 未同步（KoD 包里 stratum 会是 0），这种服务器的
	 * 时间不可信，宁可直接换下一台 */
	if (rsp[1] == 0)
		goto out;

	secs = ((uint32_t)rsp[40] << 24) | ((uint32_t)rsp[41] << 16) |
	       ((uint32_t)rsp[42] << 8) | (uint32_t)rsp[43];
	fr   = ((uint32_t)rsp[44] << 24) | ((uint32_t)rsp[45] << 16) |
	       ((uint32_t)rsp[46] << 8) | (uint32_t)rsp[47];

	/* 用 64 位做减法，顺便处理 2036 年回绕 */
	srv = (long long)secs - NTP_EPOCH_DELTA;
	if (srv < 0)
		srv += 4294967296LL;

	frac = (long long)fr * 1000 / 4294967296LL;
	srv = srv * 1000 + frac;

	/* 本地收发时刻取中点，抵消网络单程延迟
	 * （假设往返对称 —— SNTP 的标准做法） */
	l0 = (long long)t0.tv_sec * 1000 + t0.tv_usec / 1000;
	l1 = (long long)t1.tv_sec * 1000 + t1.tv_usec / 1000;
	local_mid = (l0 + l1) / 2;

	*server_ms = srv;
	*offset_ms = (long)(srv - local_mid);
	ok = true;

out:
	/* fd 和 res 在这里析构，自动 close / freeaddrinfo */
	return ok;
}

/* ------------------------------------------------------------------ */
/* HTTP Date 兜底                                                      */
/* ------------------------------------------------------------------ */

/* 公历日期 → 自 1970-01-01 起的天数（Howard Hinnant 的算法）
 *
 * 不用 timegm()：uClibc 里它不一定有，而且它按 struct tm 读，
 * 还得自己填 tm_isdst = 0，跨 libc 行为不一致。直接算最稳。 */
static long days_from_civil(int y, int m, int d)
{
	long era, yoe, doy, doe;

	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;					/* [0, 399] */
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;	/* [0, 365] */
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;		/* [0, 146096] */
	return era * 146097 + doe - 719468;
}

static int month_from_abbr(const char *s)
{
	static const char *abbr[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
	};
	int i;

	for (i = 0; i < 12; i++)
		if (strncmp(s, abbr[i], 3) == 0)
			return i + 1;
	return 0;
}

static bool http_date(long long *server_ms)
{
	struct sockaddr_in sa;
	struct timeval tv;
	char req[160], buf[1024];
	const char *p;
	char mon[8];
	int day, year, hh, mi, ss, m;
	long long epoch;
	ssize_t n, used = 0;
	res::Fd fd;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	if (inet_pton(AF_INET, "0.0.0.0", &sa.sin_addr) != 1)
		return false;

	/*
	 * 用 getaddrinfo 而不是写死 IP：CDN 的 IP 会变，写死迟早失效。
	 *
	 * ★ 这段原来最危险的地方是：getaddrinfo 的结果用 {} 包了个作用域，
	 *   里面 memcpy 完 freeaddrinfo —— 看起来没问题，但那是因为
	 *   它恰好只有一条出口。现在换成 res::AddrInfo，作用域边界不再重要。
	 *   socket fd 同理：下面 4 条 return 路径原来各写一次 close(fd)，
	 *   现在一处都不用写。
	 */
	res::AddrInfo ai;

	if (!ai.resolve(HTTP_HOST, "80", AF_INET, SOCK_STREAM))
		return false;
	memcpy(&sa, ai.get()->ai_addr, sizeof(sa));

	fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
	if (!fd.valid())
		return false;

	tv.tv_sec = 4;
	tv.tv_usec = 0;
	setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(fd.get(), (struct sockaddr *)&sa, sizeof(sa)) != 0)
		return false;

	/* HEAD 不要正文，只为了拿响应头 —— 省流量也省时间 */
	snprintf(req, sizeof(req),
		 "HEAD %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
		 HTTP_PATH, HTTP_HOST);
	if (write(fd.get(), req, strlen(req)) < 0)
		return false;

	memset(buf, 0, sizeof(buf));
	while (used < (ssize_t)sizeof(buf) - 1) {
		n = read(fd.get(), buf + used, sizeof(buf) - 1 - used);
		if (n <= 0)
			break;
		used += n;
		/* 响应头到空行就结束，不用等 EOF */
		if (strstr(buf, "\r\n\r\n"))
			break;
	}
	fd.reset();     /* 读完立刻关，后面解析不再需要 */

	p = strstr(buf, "Date: ");
	if (!p)
		return false;
	p += 6;

	/* 形如 Thu, 30 Oct 2026 00:44:42 GMT */
	if (sscanf(p, "%*3s, %d %3s %d %d:%d:%d",
		   &day, mon, &year, &hh, &mi, &ss) != 6)
		return false;

	m = month_from_abbr(mon);
	if (!m || day < 1 || day > 31 || year < 2020)
		return false;

	epoch = days_from_civil(year, m, day) * 86400LL
	      + hh * 3600LL + mi * 60LL + ss;

	*server_ms = epoch * 1000;
	return true;
}

/* ------------------------------------------------------------------ */
/* 应用与线程                                                          */
/* ------------------------------------------------------------------ */

/* 把系统时间设成 target_ms，返回设之前系统与目标的偏差（毫秒） */
static long apply_time(long long target_ms)
{
	struct timeval old, nv;
	long off;

	gettimeofday(&old, NULL);
	off = (long)(target_ms - ((long long)old.tv_sec * 1000 + old.tv_usec / 1000));

	nv.tv_sec = target_ms / 1000;
	nv.tv_usec = (target_ms % 1000) * 1000;
	if (settimeofday(&nv, NULL) != 0)
		return LONG_MIN;

	return off;
}

static void set_info(int state, long offset, const char *src, const char *err)
{
	g_lock.lock();
	g_ts.state = state;
	g_ts.offset_ms = offset;
	if (src)
		snprintf(g_ts.source, sizeof(g_ts.source), "%s", src);
	if (err)
		snprintf(g_ts.err, sizeof(g_ts.err), "%s", err);
	else
		g_ts.err[0] = '\0';
	g_ts.seq++;
	if (state == TIMESYNC_OK) {
		time_t now = time(NULL);
		struct tm lt;

		localtime_r(&now, &lt);
		strftime(g_ts.when, sizeof(g_ts.when), "%H:%M:%S", &lt);
		g_last_ok_mono_ms = mono_ms();
	}
	g_lock.unlock();
}

/* 校时线程（原来是 pthread 的 void*(void*)，参数一直没用，改成无参） */
static void sync_thread()
{
	long long srv_ms;
	long off = 0;
	char src[32] = "";
	bool ok = false;
	size_t i;

	/* ---- 主路径：SNTP ---- */
	for (i = 0; i < sizeof(g_ntp_srv) / sizeof(g_ntp_srv[0]); i++) {
		if (sntp_query(g_ntp_srv[i], 2500, &srv_ms, &off)) {
			off = apply_time(srv_ms);
			if (off == LONG_MIN)
				continue;
			snprintf(src, sizeof(src), "ntp:%s", g_ntp_srv[i]);
			ok = true;
			break;
		}
	}

	/* ---- 兜底：HTTP Date ----
	 * 企业网/公共 WiFi 常封 UDP 123，但 HTTP 一定是通的
	 * （不然这个程序的其他网络功能全都用不了）*/
	if (!ok) {
		if (http_date(&srv_ms)) {
			off = apply_time(srv_ms);
			if (off != LONG_MIN) {
				snprintf(src, sizeof(src), "http:%s", HTTP_HOST);
				ok = true;
			}
		}
	}

	if (ok) {
		set_info(TIMESYNC_OK, off, src, NULL);
		printf("[校时] 成功 来源=%s 偏差=%+ld ms\n", src, off);
	} else {
		set_info(TIMESYNC_FAILED, 0, NULL, "NTP 与 HTTP 均不可达");
		printf("[校时] 失败：NTP 与 HTTP 都拿不到时间\n");
	}

	g_lock.lock();
	g_busy = false;
	g_lock.unlock();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

extern "C" void timesync_init(void)
{
	g_lock.lock();
	memset(&g_ts, 0, sizeof(g_ts));
	g_ts.state = TIMESYNC_IDLE;
	snprintf(g_ts.err, sizeof(g_ts.err), "尚未校时");
	g_busy = false;
	g_last_ok_mono_ms = 0;
	g_lock.unlock();
}

extern "C" void timesync_start(void)
{
	g_lock.lock();

	if (g_busy) {
		g_lock.unlock();
		return;
	}

	/* 幂等：5 分钟内成功校过就不再校。
	 *
	 * 用单调时钟算间隔（见 g_last_ok_mono_ms 的注释）：
	 * 一次启动里"WiFi 连上"和"自检点重新检测"都可能调到这里，
	 * 5 分钟内不重复校时，避免反复 settimeofday。
	 *
	 * 但失败的情况不挡：失败的 5 秒后允许立刻重试
	 * （网络刚恢复时最需要它尽快生效）。 */
	if (g_ts.state == TIMESYNC_OK && g_last_ok_mono_ms > 0 &&
	    mono_ms() - g_last_ok_mono_ms < 300 * 1000) {
		g_lock.unlock();
		return;
	}

	g_busy = true;
	g_ts.state = TIMESYNC_RUNNING;
	snprintf(g_ts.err, sizeof(g_ts.err), "正在校时...");
	g_ts.seq++;
	g_lock.unlock();

	try {
		std::thread th(sync_thread);

		th.detach();
	} catch (...) {
		g_lock.lock();
		g_busy = false;
		g_ts.state = TIMESYNC_FAILED;
		snprintf(g_ts.err, sizeof(g_ts.err), "创建线程失败");
		g_ts.seq++;
		g_lock.unlock();
	}
}

extern "C" int timesync_get(struct timesync_info *out)
{
	g_lock.lock();
	*out = g_ts;
	g_lock.unlock();
	return out->seq;
}

extern "C" int timesync_trusted(void)
{
	int r;

	g_lock.lock();
	r = (g_ts.state == TIMESYNC_OK) ? 1 : 0;
	g_lock.unlock();
	return r;
}
