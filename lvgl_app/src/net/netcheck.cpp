/**
 * netcheck.c —— 五层网络自检
 *
 * 每层做完立刻更新结果（不是等全部做完才更新），
 * 这样 UI 能一步步看着进度走，用户不会觉得卡住了。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdbool>
#include <unistd.h>
#include <time.h>

#include <mutex>
#include <thread>

#include "netcheck.h"
#include "timesync.h"
#include "resources.hpp"     /* res::Pipe —— popen 的 RAII 封装 */

static struct netcheck_result g_nc;
static std::mutex g_lock;
static bool g_busy;

#define IFNAME "wlan0"

/* 毫秒计时 */
static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 跑命令收集输出（内部工具，输出都很短） */
/*
 * 跑一条 shell 命令，把输出读进 out。
 *
 * ★ 原来是手写 popen/fread/pclose。换成 res::run_shell 之后，
 *   "popen 失败" 和 "读一半" 这两条路径都不需要各自 pclose ——
 *   Pipe 析构时自动关（并且 waitpid，不留僵尸进程）。
 *
 *   注意 popen 必须配 pclose 不能用 fclose：后者不 waitpid，
 *   子进程会变 <defunct>。这台板上网络自检是反复执行的，
 *   攒几十个僵尸进程会把 PID 表占满。
 */
static void sh(const char *cmd, char *out, size_t outsz)
{
	(void)res::run_shell(cmd, out, outsz);
}

/* 更新某一步的结果（加锁），并立刻对外可见 */
static void set_step(int idx, int ok, const char *detail, int ms)
{
	g_lock.lock();
	/* 步骤名（steps[idx].name）是线程开始时填好的，这里不动它 */
	g_nc.steps[idx].ok = ok;
	snprintf(g_nc.steps[idx].detail, sizeof(g_nc.steps[idx].detail),
		 "%s", detail ? detail : "");
	g_nc.steps[idx].ms = ms;
	if (ok)
		g_nc.done = idx + 1;
	g_nc.seq++;
	g_lock.unlock();
}

/* 把 /proc/net/arp 里某个 IP 的 MAC 提取出来；未解析则返回 false */
static bool arp_resolved(const char *ip, char *mac, size_t macsz)
{
	char out[1024], *line, *next;

	mac[0] = '\0';
	sh("cat /proc/net/arp 2>/dev/null", out, sizeof(out));

	for (line = out; line && *line; line = next) {
		char ipfield[32], hw[32], flags[16], macf[32], mask[16], dev[16];

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		/* 格式：IPaddress HWtype Flags HWaddress Mask Device */
		if (sscanf(line, "%31s %31s %15s %31s %15s %15s",
			   ipfield, hw, flags, macf, mask, dev) != 6)
			continue;
		if (strcmp(ipfield, ip) != 0)
			continue;

		/* flags 0x2 = ATF_COM（已完成地址解析） */
		if (strstr(flags, "0x2")) {
			snprintf(mac, macsz, "%s", macf);
			return true;
		}
		return false;
	}
	return false;
}

/* 取默认网关 */
static bool default_gw(char *gw, size_t gsz){
	char out[512], *line, *next;

	gw[0] = '\0';
	sh("ip route 2>/dev/null", out, sizeof(out));

	for (line = out; line && *line; line = next) {
		char *via;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (strncmp(line, "default", 7) != 0)
			continue;
		if (!strstr(line, IFNAME))
			continue;
		via = strstr(line, "via ");
		if (!via)
			continue;
		via += 4;

		size_t i = 0;

		while (*via && *via != ' ' && i + 1 < gsz)
			gw[i++] = *via++;
		gw[i] = '\0';
		return gw[0] != '\0';
	}
	return false;
}

/* 后台线程 */
/*
 * 自检线程。
 *
 * ★ 原来是 static void *check_thread(void *arg) + return NULL。
 *   改成普通函数后用 std::thread 启动。
 */
static void check_thread()
{
	char out[1024], buf[128];
	long t0;
	int i;

	/* 初始化步骤名（结果结构里名字是固定的，先填好） */
	g_lock.lock();
	for (i = 0; i < NETCHECK_STEPS; i++) {
		g_nc.steps[i].ok = -1;
		g_nc.steps[i].detail[0] = '\0';
		g_nc.steps[i].ms = 0;
	}
	snprintf(g_nc.steps[0].name, sizeof(g_nc.steps[0].name), "关联");
	snprintf(g_nc.steps[1].name, sizeof(g_nc.steps[1].name), "IP地址");
	snprintf(g_nc.steps[2].name, sizeof(g_nc.steps[2].name), "网关");
	snprintf(g_nc.steps[3].name, sizeof(g_nc.steps[3].name), "DNS");
	snprintf(g_nc.steps[4].name, sizeof(g_nc.steps[4].name), "外网");
	snprintf(g_nc.steps[5].name, sizeof(g_nc.steps[5].name), "时间");
	g_nc.done = 0;
	g_nc.summary[0] = '\0';
	g_nc.seq++;
	g_lock.unlock();

	/* ---------- 1. 关联 ---------- */
	t0 = now_ms();
	sh("wpa_cli -i " IFNAME " status 2>&1", out, sizeof(out));
	if (strstr(out, "wpa_state=COMPLETED")) {
		/* 顺便把 SSID 抓出来做细节（wpa_cli 输出是 \xNN 转义的，
		 * 这里只显示信号强度更有意义） */
		sh("iw dev " IFNAME " link 2>&1", out, sizeof(out));
		{
			const char *p = strstr(out, "signal:");
			char sig[24] = "";

			if (p) {
				p += 7;
				while (*p == ' ')
					p++;
				size_t k = 0;

				while (*p && *p != '\n' && *p != ' ' &&
				       k + 1 < sizeof(sig))
					sig[k++] = *p++;
				sig[k] = '\0';
			}
			snprintf(buf, sizeof(buf), "%s", sig[0] ? sig : "已关联");
		}
		set_step(0, 1, buf, (int)(now_ms() - t0));
	} else {
		set_step(0, 0, "未关联 WiFi", (int)(now_ms() - t0));
		goto finish;
	}

	/* ---------- 2. IP ---------- */
	t0 = now_ms();
	sh("ifconfig " IFNAME " 2>&1", out, sizeof(out));
	{
		const char *p = strstr(out, "inet addr:");
		char ip[32] = "";

		if (p) {
			p += 10;
			size_t k = 0;

			while (*p && *p != ' ' && k + 1 < sizeof(ip))
				ip[k++] = *p++;
			ip[k] = '\0';
		}
		if (ip[0]) {
			snprintf(buf, sizeof(buf), "%s", ip);
			set_step(1, 1, buf, (int)(now_ms() - t0));
		} else {
			set_step(1, 0, "没有 IP", (int)(now_ms() - t0));
			goto finish;
		}
	}

	/* ---------- 3. 网关（路由 + ARP）---------- */
	t0 = now_ms();
	{
		char gw[32], mac[32];

		if (!default_gw(gw, sizeof(gw))) {
			set_step(2, 0, "无默认路由", (int)(now_ms() - t0));
			goto finish;
		}
		if (!arp_resolved(gw, mac, sizeof(mac))) {
			/* 这一层是最容易"看起来正常实际不通"的地方 */
			snprintf(buf, sizeof(buf), "%s ARP未解析", gw);
			set_step(2, 0, buf, (int)(now_ms() - t0));
			goto finish;
		}
		snprintf(buf, sizeof(buf), "%s", gw);
		set_step(2, 1, buf, (int)(now_ms() - t0));
	}

	/* ---------- 4. DNS ----------
	 * ★ 不要用 nslookup 测 DNS！实测它固定要 5 秒，
	 *   而且【指定服务器也慢】（nslookup www.baidu.com 10.141.62.128 同样 5.03s），
	 *   说明是 busybox nslookup 自己的问题（多半在做反向解析或重试），
	 *   不是 DNS 服务慢 —— 同一个域名 wget 只要 311ms。
	 *
	 * 更可靠的判据：用 wget 请求一个【域名】（不是 IP）。
	 * -S 让它把 HTTP 响应头打到 stderr，有 "HTTP/" 就说明
	 * 域名解析成功 + TCP 连上了。
	 *
	 * ★ 第 4、5 步共用这一次请求，不重复发：
	 *   响应头和响应体一起收进 out，
	 *   第 4 步看 "HTTP/" 判断"解析+连接"，
	 *   第 5 步看响应体里的 "status":200 判断"应用层协议通"。
	 *   之前分两次 wget 是白花了两倍时间。
	 */
	t0 = now_ms();
	sh("wget -q -S -T 6 -t 1 -O - "
	   "\"http://t.weather.itboy.net/api/weather/city/101280101\" 2>&1",
	   out, sizeof(out));
	{
		int ms_dns = (int)(now_ms() - t0);

		if (!strstr(out, "HTTP/")) {
			set_step(3, 0, "域名解析失败", ms_dns);
			goto finish;
		}
		/* DNS + TCP 都通了，这一步的"耗时"就按整次请求算 */
		set_step(3, 1, "域名解析正常", ms_dns);

		/* ---------- 5. 外网 HTTP（复用上面的响应）---------- */
		if (strstr(out, "\"status\":200") || strstr(out, "success")) {
			set_step(4, 1, "HTTP 内容正常", 0);
		} else {
			set_step(4, 0, "响应内容异常", 0);
		}
	}

	/* ---------- 6. 时间 ----------
	 * 只报告状态，不在这里触发校时：
	 * 自检是"诊断"动作，不该带副作用（改系统时间是很重的副作用）。
	 * 校时的触发点是 WiFi 连上 + 主循环巡检，各司其职。
	 *
	 * 但如果发现没同步，顺手把它叫起来 —— 这样用户点一次"重新自检"，
	 * 等于手动催了一次校时，是个符合直觉的交互。
	 */
	t0 = now_ms();
	{
		struct timesync_info ti;

		timesync_get(&ti);

		if (ti.state == TIMESYNC_OK) {
			/* 只报告核心信息：本地时间 + 走了 NTP 还是 HTTP。
			 * 不拼完整服务器名 —— 那一行宽度只有约 200px，
			 * 拼上 "ntp.aliyun.com" 会被截成 "ntp:ntp.aliyu..."，
			 * 反而看不出重点。 */
			const char *via = strncmp(ti.source, "ntp", 3) == 0 ?
					  "NTP" : "HTTP";

			if (ti.offset_ms > -200 && ti.offset_ms < 200)
				snprintf(buf, sizeof(buf), "%s %s",
					 ti.when, via);
			else
				snprintf(buf, sizeof(buf), "%s %s %+ldms",
					 ti.when, via, ti.offset_ms);
			set_step(5, 1, buf, (int)(now_ms() - t0));
		} else if (ti.state == TIMESYNC_RUNNING) {
			set_step(5, -1, "正在校时", 0);
			timesync_start();
		} else {
			set_step(5, 0, ti.err[0] ? ti.err : "未同步", 0);
			/* 没同步就催一次，下次进来就能看到结果 */
			timesync_start();
		}
	}

finish:
	g_lock.lock();
	{
		int pass = 0, total = NETCHECK_STEPS;

		for (i = 0; i < total; i++)
			if (g_nc.steps[i].ok == 1)
				pass++;

		if (pass == total)
			snprintf(g_nc.summary, sizeof(g_nc.summary),
				 "全部 %d 项通过，可以上网", total);
		else
			snprintf(g_nc.summary, sizeof(g_nc.summary),
				 "%d/%d 项通过，卡在「%s」", pass, total,
				 g_nc.steps[pass].name);
	}
	g_nc.state = NETCHECK_DONE;
	g_nc.seq++;
	g_busy = false;
	g_lock.unlock();

	printf("[自检] %s\n", g_nc.summary);
}

extern "C" void netcheck_init(void)
{
	memset(&g_nc, 0, sizeof(g_nc));
	g_nc.state = NETCHECK_IDLE;
	g_busy = false;
}

extern "C" void netcheck_start(void)
{

	g_lock.lock();
	if (g_busy) {
		g_lock.unlock();
		return;
	}
	g_busy = true;
	g_nc.state = NETCHECK_RUNNING;
	g_nc.seq++;
	g_lock.unlock();

	try {
		std::thread th(check_thread);

		th.detach();
	} catch (...) {
		g_lock.lock();
		g_busy = false;
		g_nc.state = NETCHECK_DONE;
		snprintf(g_nc.summary, sizeof(g_nc.summary), "创建自检线程失败");
		g_lock.unlock();
		return;
	}
}

extern "C" int netcheck_get(struct netcheck_result *out)
{
	int seq;

	g_lock.lock();
	memcpy(out, &g_nc, sizeof(*out));
	seq = g_nc.seq;
	g_lock.unlock();

	return seq;
}
