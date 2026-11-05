/**
 * wifi_conn.c —— 用 wpa_cli + udhcpc 完成真实 WiFi 连接
 *
 * 整个流程分四步，每一步都可能失败，所以状态要细分，
 * 出错时能告诉用户"卡在哪一步"而不是笼统的"连接失败"：
 *
 *   1. 确保 wpa_supplicant 在跑          → stage="启动服务"
 *   2. 下发 ssid/psk 并 select_network    → stage="下发配置"
 *   3. 轮询 wpa_state 直到 COMPLETED      → stage="关联中"
 *   4. udhcpc 拿 IP                       → stage="获取IP"
 *
 * 【踩过的坑】
 * a) SSID 用 hex 传而不是带引号的字符串。
 *    中文 SSID 带引号传时，wpa_cli 会因为编码问题处理不一致；
 *    hex 形式（e5b08fe6a183...）是纯 ASCII，怎么都对。
 * b) psk 里的 " 和 \ 必须转义，否则命令行会截断。
 * c) 关联完成 ≠ 能上网。wpa_state=COMPLETED 只说明四次握手成功，
 *    还要 DHCP 拿到 IP 才真的能用。这两步分开判断。
 * d) udhcpc 要用 -n -t N（拿不到就退出），否则它会在前台一直等，
 *    把后台线程挂在那里。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdbool>
#include <cctype>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>		/* realpath / PATH_MAX */

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "resources.hpp"	/* res::Pipe / res::File / res::run_argv */

#include "wifi_conn.h"
#include "../net/timesync.h"

static struct wifi_conn_info g_ci;
static std::mutex g_lock;
static bool g_busy;		/* 有线程在干活，防止重复启动 */

#define IFNAME     "wlan0"

/*
 * wpa_supplicant 的持久配置路径。
 *
 * ★ 为什么不用默认的 /etc/wpa_supplicant.conf：
 *   ① rootfs（ubifs，30.2M）曾经被占满过 —— save_config 在空间不足时
 *      只是返回 FAIL，不报任何原因，配置就默默丢了；
 *   ② 这套固件喜欢把 /etc 下的东西往 tmpfs 上引（/etc/resolv.conf
 *      就是符号链接指向 ../tmp/resolv.conf），tmpfs 重启清空。
 *      把 WiFi 密码放在一个"可能是 tmpfs"的地方，重启必丢。
 *
 *   /userdata 是 ubifs 上专门的可写分区（iq 参数、驱动标记都放这），
 *   容量比 rootfs 宽裕，且确定持久。
 *
 *   首次运行时从 /etc 那份复制过来，保留出厂设置（ctrl_interface 等）。
 */
#define WPA_CONF          "/userdata/wpa_supplicant.conf"
#define WPA_CONF_ORIGIN   "/etc/wpa_supplicant.conf"

/* 实际生效的配置路径：/userdata 写不进去时会退到 /etc，别让 WiFi 直接不可用 */
static char g_wpa_conf[80] = WPA_CONF;

/* 定义在文件后面（持久化那一节），但 conn_thread 要用 */
static int ensure_wpa_conf(void);
static bool save_wpa_config(void);
#define ASSOC_TIMEOUT_S   25	/* 关联最长等多久 */
#define ASSOC_POLL_MS     400	/* 关联状态轮询间隔 */

/* 把字符串转成十六进制（SSID 用） */
static void to_hex(const char *in, char *out, size_t outsz)
{
	static const char *H = "0123456789abcdef";
	size_t i, o = 0;

	for (i = 0; in[i] && o + 2 < outsz; i++) {
		unsigned char c = (unsigned char)in[i];

		out[o++] = H[c >> 4];
		out[o++] = H[c & 0x0f];
	}
	out[o] = '\0';
}

/**
 * 执行命令并把全部输出读进 buf
 *
 * 返回值：命令退出码。失败要看 buf 里的错误信息。
 */
static int run_cmd(const char *cmd, char *buf, size_t bufsz)
{
	/*
	 * ★ 原来这里是手写的 popen / fread / pclose 三段式。
	 *   换成 res::run_shell 后，"popen 失败"、"读一半出错" 这些路径
	 *   都不再需要各自记得 pclose —— Pipe 析构时自动关。
	 *
	 *   ★ 注意 popen 必须配 pclose，不能用 fclose：
	 *     fclose 只关 FILE，不 waitpid，子进程会变僵尸（<defunct>）。
	 *     原来的代码是对的，但这类"配对"靠人记，改成 RAII 才不会再错。
	 */
	return res::run_shell(cmd, buf, bufsz);
}

/**
 * ★★★ 不经 shell 执行命令（argv 数组直接 exec）
 *
 * 【为什么必须有这个函数 —— 这是我踩的最大的坑】
 *
 * popen() 走的是 `/bin/sh -c`，也就是命令字符串会被 shell 解析一遍。
 * 而 wpa_cli 下发密码的语法要求值本身带引号：
 *     wpa_cli set_network 0 psk "mypassword"
 * shell 会把这层引号【吃掉】，wpa_cli 实际收到的是裸的 mypassword。
 * wpa_supplicant 拿到不带引号的值，会按"64 位十六进制 PSK"去解析，
 * 8 个字符当然不合法 → 返回 FAIL → 报"密码长度需 8-63 位"。
 *
 * 也就是说：**用户密码完全正确，也会被判成"密码无效"**。
 * 我一开始就被这个报错误导，往密码方向查了半天。
 *
 * 实测对比（板上验证）：
 *     wpa_cli ... psk "12345678"      → FAIL   ← shell 吃掉引号
 *     wpa_cli ... psk "\"12345678\""  → OK     ← 双层引号
 *
 * 双层引号能"绕"过去，但只要密码里含 $ ` \ ' 空格 等 shell 元字符，
 * 仍会被 shell 解释（比如 $HOME 会被展开成路径）。
 * 正解是绕开 shell：fork 之后直接 execvp，argv 逐个传递，
 * 引号和特殊字符原样到达目标程序。
 *
 * 顺带这也消除了命令注入风险 —— 密码里写 `; rm -rf /` 也只会
 * 被当成密码字符串，不会被当命令执行。
 */
/**
 * 执行 `wpa_cli -i wlan0 <参数...>`，最多 5 个参数
 *
 * 用绝对路径 /usr/sbin/wpa_cli：init 脚本拉起的进程 PATH 可能不全，
 * execvp 靠 PATH 查找有找不到的风险。
 */
static int wpa_cli_run(char *buf, size_t bufsz,
		       const char *a1, const char *a2,
		       const char *a3, const char *a4,
		       const char *a5)
{
	/*
	 * ★ C++ 里字符串字面量是 const char[]，不能隐式转成 char*
	 *   （C 允许，C++ 报 -Wwrite-strings）。
	 *   而 execvp 的签名历史上就是 char *const argv[]（它不修改内容）。
	 *   所以这里用 const char* 存，最后再 const_cast 一次交给 execvp ——
	 *   集中在一处取消 const，比到处散落 (char*) 强转好维护。
	 */
	const char *argv[10];
	int i = 0;

	argv[i++] = "/usr/sbin/wpa_cli";
	argv[i++] = "-i";
	argv[i++] = IFNAME;
	if (a1) argv[i++] = (char *)a1;
	if (a2) argv[i++] = (char *)a2;
	if (a3) argv[i++] = (char *)a3;
	if (a4) argv[i++] = (char *)a4;
	if (a5) argv[i++] = (char *)a5;
	argv[i] = nullptr;

	return res::run_argv(argv, buf, bufsz, /* merge_stderr = */ true);
}

/* decode_escaped 定义在文件后面，这里先用 */
static void decode_escaped(const char *in, char *out, size_t outsz);

/**
 * 按 SSID 查出已存在的 network id。
 *
 * ★ 为什么需要它 —— 这是"保存"这件事的第二个坑：
 *
 *   add_network 每次都新建一个 network 块，而 save_config 会把内存里
 *   的全部写进配置文件。于是"连同一个 WiFi 三次"就在文件里留下三个
 *   一模一样的 network={...}，实测第一次就出现了两个：
 *
 *       network={ ssid=e5b08f... psk="..." disabled=1 }   ← 出厂那个，被 select 禁用
 *       network={ ssid=e5b08f... psk="..." }              ← 新加的
 *
 *   文件变脏只是表象，真正的风险是【重启时 wpa_supplicant 面对多个
 *   同 SSID 的网络块，可能挑中 disabled 的那个然后根本不连】——
 *   表现又回到"保存了但连不上"。
 *
 *   所以：已存在就复用它的 id（set_network 会覆盖旧密码，改密码也没问题），
 *   只有全新网络才 add_network。
 *
 * @param disabled 输出该网络当前是否 [DISABLED]（可为 nullptr）
 * @return network id；没找到返回 -1
 */
static int find_network_by_ssid(const char *ssid, bool *disabled)
{
	char out[2048];
	char *line, *next;
	int found = -1;

	wpa_cli_run(out, sizeof(out), "list_networks", NULL, NULL, NULL, NULL);

	for (line = out; line && *line; line = next) {
		char decoded[WIFI_SSID_MAX * 4];
		char *p, *tab;
		size_t oi = 0;
		int id;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		/* 表头 "network id / ssid / bssid / flags" */
		if (strncmp(line, "network id", 10) == 0)
			continue;

		tab = strchr(line, '\t');
		if (!tab)
			continue;

		id = atoi(line);		/* 行首就是 id */
		p = tab + 1;			/* 跳过 tab，指向 ssid */

		/* wpa_cli 把非 ASCII 的 SSID 输出成 \xNN 转义，要解码后才能比 */
		while (*p && *p != '\t' && oi + 1 < sizeof(decoded)) {
			if (p[0] == '\\' && p[1] == 'x' &&
			    isxdigit((unsigned char)p[2]) &&
			    isxdigit((unsigned char)p[3])) {
				char hex[3] = { p[2], p[3], 0 };

				decoded[oi++] = (char)strtol(hex, NULL, 16);
				p += 4;
			} else {
				decoded[oi++] = *p++;
			}
		}
		decoded[oi] = '\0';

		if (strcmp(decoded, ssid) == 0) {
			found = id;
			if (disabled)
				*disabled = (strstr(line, "[DISABLED]") != NULL);
			break;
		}
	}

	return found;
}

/* 更新状态（加锁） */
static void set_state(int state, const char *stage, const char *err,
		      const char *ssid, const char *ip)
{
	g_lock.lock();
	g_ci.state = state;
	if (stage)
		snprintf(g_ci.stage, sizeof(g_ci.stage), "%s", stage);
	if (err)
		snprintf(g_ci.err, sizeof(g_ci.err), "%s", err);
	else
		g_ci.err[0] = '\0';
	if (ssid)
		snprintf(g_ci.ssid, sizeof(g_ci.ssid), "%s", ssid);
	if (ip)
		snprintf(g_ci.ip, sizeof(g_ci.ip), "%s", ip);
	g_ci.seq++;

	/* ★ 失败时一定打日志。
	 * 之前只在 set_state 里改 UI 状态、不打印，结果连接失败时
	 * 串口日志一片安静 —— 只能看到"收到密码，开始连接"，
	 * 后面是成功还是失败、卡在哪一步，全靠猜。
	 * 现在所有失败分支都会在这里留下痕迹。 */
	if (state == WIFI_CONN_FAILED)
		printf("[WiFi] 连接失败: 阶段=%s 原因=%s (目标=%s)\n",
		       stage ? stage : "-", err ? err : "-", g_ci.ssid);

	g_lock.unlock();
}

/**
 * 把本网卡对应的 nameserver 提到 resolv.conf 最前面
 *
 * 【为什么需要】
 * busybox 的 udhcpc 脚本用 `echo "nameserver $i" >> $RESOLV_CONF`
 * **追加**，不是替换。所以连过几次不同网络后，resolv.conf 会变成：
 *     nameserver 223.5.5.5             ← 很早以前的，还排在第一个
 *     nameserver 10.141.62.128 # wlan0 ← 当前网络的
 *
 * 解析器是**按顺序**试的：先问 223.5.5.5（公网，RTT 50ms+），
 * 慢了才轮到本地网关（RTT 5ms）。实测 nslookup 因此固定要 4.9 秒。
 *
 * 【★ 踩的第二个坑：resolv.conf 是符号链接】
 * 板上是：
 *     /etc/resolv.conf -> ../tmp/resolv.conf
 * 也就是说真实文件在 /tmp（tmpfs）。
 * 我第一版直接 rename("/tmp/x", "/etc/resolv.conf")，
 * 会把【符号链接本身】替换成普通文件 —— 而且 /etc 在只读的 ubifs 根分区上，
 * rename 直接失败，于是什么也没改，还不报错。
 *
 * 正确做法：先 realpath() 解析出真实路径，再用它。
 */
/*
 * 把本网卡的 DNS 条目提到 resolv.conf 最前面。
 *
 * 【为什么要做】udhcpc 用 >> 追加写 resolv.conf，旧 DNS 永远排第一，
 * 换网络之后就会一直用已经失效的 DNS。
 *
 * ★ 原来这里有 4 个 fopen 和 6 处 fclose，分布在 4 条 return 路径上：
 *     第 1 遍 in 失败 → return（此时 out 还没开，漏不了）
 *     out 失败 → fclose(in)         ← 手写配对
 *     两遍正常 → 各 fclose(in)+fclose(out)
 *     rename 失败降级 → src/out 各一对
 *   漏一处就是 FILE 泄漏 + 缓冲区内存泄漏。而且这个函数在每次连上
 *   网络时都会跑一次，属于"高频路径"。
 *
 *   改成 res::File 之后，上面那些 fclose 全部消失 —— 没有任何一条
 *   路径需要写清理代码。
 */
static void reorder_resolv(const char *ifname)
{
	char line[128], tag[64];
	char real[PATH_MAX];
	const char *conf;

	std::snprintf(tag, sizeof(tag), "# %s", ifname);

	/* 解析符号链接，拿到真正要写的文件 */
	if (::realpath("/etc/resolv.conf", real))
		conf = real;
	else
		conf = "/etc/resolv.conf";      /* 不是链接，或解析失败就直用 */

	/*
	 * 两遍扫描：第 1 遍只写本网卡的，第 2 遍写其余的。
	 * 每遍都从原文件重新读，所以先把内容烤进内存更省事 ——
	 * 但 resolv.conf 只有几行，重开两次文件无所谓。
	 */
	for (int i = 0; i < 2; i++) {
		res::File in(std::fopen(conf, "r"));

		if (!in)
			return;

		res::File out(std::fopen("/tmp/.resolv.new", i == 0 ? "w" : "a"));
		if (!out)
			return;         /* in 由析构自动关，这里不用管 */

		while (std::fgets(line, sizeof(line), in.get())) {
			bool mine = std::strstr(line, tag) != nullptr;

			if ((i == 0) != mine)   /* 第 1 遍只要本网卡的 */
				continue;
			if (line[0] == '\n')
				continue;       /* 清掉空行 */
			std::fputs(line, out.get());
		}
		/* in / out 在这里析构 → 自动 fclose */
	}

	/* 写回真实路径（不是 /etc/resolv.conf 这个链接） */
	if (::rename("/tmp/.resolv.new", conf) != 0) {
		/*
		 * rename 跨文件系统会失败（/tmp 和 /etc 若不在同一 fs）。
		 * 那就退化成"拷贝内容"：不换 inode，只覆盖内容。
		 */
		char buf[512];
		size_t n;
		res::File src(std::fopen("/tmp/.resolv.new", "r"));

		if (!src)
			return;

		res::File out(std::fopen(conf, "w"));
		if (!out)
			return;

		while ((n = std::fread(buf, 1, sizeof(buf), src.get())) > 0)
			std::fwrite(buf, 1, n, out.get());
		/* 先关 src 再 unlink 更稳（虽然 Linux 允许） */
		src.reset();
		out.reset();
		::unlink("/tmp/.resolv.new");
	}
}

/* 从 "inet addr:192.168.2.139" 里把 IP 抠出来 */
static void parse_ip(const char *ifconfig_out, char *ip, size_t ipsz)
{
	const char *p = strstr(ifconfig_out, "inet addr:");

	ip[0] = '\0';
	if (!p)
		return;
	p += strlen("inet addr:");

	size_t i = 0;

	while (*p && *p != ' ' && *p != '\n' && i + 1 < ipsz)
		ip[i++] = *p++;
	ip[i] = '\0';
}

/**
 * 取某网卡当前的默认网关地址
 *
 * 读 `ip route` 的输出，找 "default via X.X.X.X dev <ifname>"。
 * 不用 ioctl(SIOCGATEWAY)：那个接口还得自己拼 struct rtentry，
 * 解析文本在这个场景下更直白。
 */
static bool get_default_gw(const char *ifname, char *gw, size_t gws)
{
	char out[512], *line, *next;
	gw[0] = '\0';
	run_cmd("ip route 2>&1", out, sizeof(out));

	for (line = out; line && *line; line = next) {
		char *via;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (strncmp(line, "default", 7) != 0)
			continue;
		if (!strstr(line, ifname))
			continue;

		via = strstr(line, "via ");
		if (!via)
			continue;
		via += 4;

		size_t i = 0;

		while (*via && *via != ' ' && i + 1 < gws)
			gw[i++] = *via++;
		gw[i] = '\0';
		return gw[0] != '\0';
	}
	return false;
}

/**
 * 确保 wlan0 上有一个"当前网络真能用"的 IP；没有就重新 DHCP。
 *
 * ★ 为什么不能只看 `ifconfig` 有没有 inet addr：
 *   Linux 的 IP 配置不会因为 wpa_supplicant 换了 AP 就自动清掉。
 *   从一个热点切到另一个（实测：192.168.2.x 的小桃-2.4G
 *   → 10.153.27.x 的小桃）时，wlan0 上【还挂着上一个网络的地址】。
 *
 *   于是"有 IP 就跳过 DHCP"会漏掉最关键的一步：
 *     · 没拿到新网段的地址
 *     · 没有指向新网关的 default 路由
 *     · /etc/resolv.conf 里没有新 DNS
 *   结果就是用户看到的现象：**WiFi 明明连上了，但 DNS 错误**。
 *   而 wpa_cli status 显示 COMPLETED，看起来一切正常，极具误导性。
 *
 *   这个 bug 在开机脚本和应用层各有一份（都是"只看有没有 IP"），
 *   现在统一用这个函数判定。
 *
 * 判据（两条都满足才算可用）：
 *   ① 有 default 路由指向 wlan0
 *   ② 那个网关的 ARP 已解析（/proc/net/arp 里 flags 含 0x2）
 *
 * @param ip    输出缓冲：最终可用的 IP（拿不到就是空串）
 * @param ipsz  ip 大小
 * @return true = 有可用 IP
 */
static bool ensure_usable_ip(char *ip, size_t ipsz)
{
	bool ip_usable = false;
	char out[512];

	run_cmd("ifconfig " IFNAME " 2>&1", out, sizeof(out));
	parse_ip(out, ip, ipsz);

	if (ip[0] != '\0') {
		char gw[32] = "", arp[512], *line, *next;

		if (get_default_gw(IFNAME, gw, sizeof(gw))) {
			run_cmd("cat /proc/net/arp 2>&1", arp, sizeof(arp));
			for (line = arp; line && *line; line = next) {
				next = strchr(line, '\n');
				if (next)
					*next++ = '\0';
				if (!strstr(line, gw))
					continue;
				if (!strstr(line, IFNAME))
					continue;
				/* flags 是第 3 段，0x2 = 已完成 ARP 解析 */
				if (strstr(line, "0x2"))
					ip_usable = true;
				break;
			}
			if (!ip_usable)
				printf("[WiFi] 现有 IP %s 的网关 %s ARP 未解析，重新 DHCP\n",
				       ip, gw);
		} else {
			printf("[WiFi] 现有 IP %s 没有默认路由，重新 DHCP\n", ip);
		}
	}

	if (ip_usable)
		return true;

	/* ★ 先用 ifconfig 清掉旧地址，再 DHCP。
	 *
	 * 【踩过的坑：udhcpc 的 -R 不是"清旧地址"，是"退出时释放 IP"】
	 *   一开始这里写的是 `udhcpc -R -n -q`，想当然以为 -R 会先释放旧租约。
	 *   实际上 -R 是 Release-on-exit，而 -q 是"拿到租约就退出" ——
	 *   两个凑一起 = 刚拿到 IP 立刻自己退掉，板上日志是：
	 *       udhcpc: lease of 192.168.2.139 obtained ...
	 *       deleting routers / adding dns 192.168.2.1
	 *       udhcpc: entering released state     ← 地址没了
	 *   结果就是"关联成功、界面显示已连接，但没 IP 没路由没 DNS"，
	 *   和用户报的"WiFi 连了但 DNS 错误"一模一样。
	 *   清旧地址要用 ifconfig 0.0.0.0，别指望 -R。
	 *
	 * -n 拿不到就退出 / -t 5 试 5 次 / -T 3 每次 3 秒
	 * 不加 -n 它会一直前台等，把后台线程挂死 */
	run_cmd("ifconfig " IFNAME " 0.0.0.0 2>&1", NULL, 0);
	run_cmd("udhcpc -i " IFNAME " -n -t 5 -T 3 -q 2>&1",
		out, sizeof(out));
	run_cmd("ifconfig " IFNAME " 2>&1", out, sizeof(out));
	parse_ip(out, ip, ipsz);

	/* ★ 这里【故意】不重新要求 ARP 已解析。
	 *   DHCP 刚拿到地址的那一刻，网关的 ARP 条目还没建立，
	 *   拿它当失败判据会把正常情况误报成"DHCP 失败"。
	 *   所以 DHCP 之后只要拿到地址就算成功，路由/ARP 由
	 *   后面真正发包时去建立 —— 这正是 DHCP 的正常时序。 */
	return ip[0] != '\0';
}

/**
 * /etc/resolv.conf 没有 nameserver 时，用网关兜底。
 *
 * /etc/resolv.conf 是指向 ../tmp/resolv.conf 的符号链接，/tmp 是 tmpfs
 * —— **重启后是空的**。如果 DHCP 没写成，全系统就没 DNS：
 * 能 ping 通 IP，但所有域名解析失败，症状就是"DNS 错误"。
 * 家用路由器绝大多数兼做 DNS 代理，拿网关顶上足够。
 */
static void ensure_dns(void)
{
	char out[512];

	run_cmd("grep -c '^nameserver' /etc/resolv.conf 2>&1", out, sizeof(out));
	if (atoi(out) > 0)
		return;

	char gw[32] = "";

	if (!get_default_gw(IFNAME, gw, sizeof(gw)))
		return;

	char cmd[128];

	std::snprintf(cmd, sizeof(cmd), "echo 'nameserver %s' >> /etc/resolv.conf",
		      gw);
	run_cmd(cmd, NULL, 0);
	printf("[WiFi] resolv.conf 里没有 DNS，已用网关 %s 兜底\n", gw);
}

/* 查 wpa_state */
static void query_state(char *state, size_t statesz, char *ip, size_t ipsz)
{
	char out[1024];
	const char *p;

	state[0] = '\0';
	if (ip)
		ip[0] = '\0';

	wpa_cli_run(out, sizeof(out), "status", NULL, NULL, NULL, NULL);

	p = strstr(out, "wpa_state=");
	if (p) {
		size_t i = 0;

		p += strlen("wpa_state=");
		while (*p && *p != '\n' && i + 1 < statesz)
			state[i++] = *p++;
		state[i] = '\0';
	}

	/* status 里也可能带 ip_address= */
	if (ip) {
		p = strstr(out, "ip_address=");
		if (p) {
			size_t i = 0;

			p += strlen("ip_address=");
			while (*p && *p != '\n' && i + 1 < ipsz)
				ip[i++] = *p++;
			ip[i] = '\0';
		}
	}
}

/* 后台线程：真正干活的地方 */
/*
 * 连接线程。
 *
 * ★ 参数从"打包的 char** + 手工 free"改成按值传 std::string：
 *   原来 wifi_conn_start 里 malloc(2*sizeof(char*)) + strdup×2，
 *   线程里 free×3。三块内存、五个分配点、两条失败路径 ——
 *   只要有一条没清干净，就是每次连接尝试漏一次。
 *   而 WiFi 连接是典型的"反复重试"操作（密码错时用户会连点十几次）。
 *
 *   现在：值捕获进 lambda，生命周期由线程自己的栈帧保证。
 *   函数返回时自动释放，不需要写任何 free。
 */
static void conn_thread(std::string ssid_holder, std::string psk_holder)
{
	/*
	 * ★ 函数体里有几十处把 ssid/psk 当 const char* 用（set_state、
	 *   to_hex、snprintf、strlen…）。改成 std::string 之后那些调用
	 *   全都要 .c_str()，改动面大且容易漏。
	 *
	 *   这里起两个同名的局部指针指向 string 内部缓冲 ——
	 *   函数体一个字都不用动，而所有权仍然是 std::string 的
	 *   （真正的生命周期管理才是这次转换的目的）。
	 *
	 *   注意：必须在整个函数执行期间保持 ssid_holder / psk_holder 存活，
	 *   所以它们是按值传进来的参数，而不是临时对象。
	 */
	const char *ssid = ssid_holder.c_str();
	const char *psk = psk_holder.empty() ? nullptr : psk_holder.c_str();
	char hex[2 * WIFI_SSID_MAX + 8];
	char psk_arg[2 * WIFI_PSK_MAX + 8];
	char id_str[16], out[1024];
	char st[32], ip[16];
	int id = -1, waited_ms = 0;
	bool ok = false;
	bool reused = false;	/* 复用了已存在的 network 而不是新建 */

	/* ---- 步骤 1：确保 wpa_supplicant 在跑 ---- */
	set_state(WIFI_CONN_WORKING, "启动服务", NULL, ssid, "");

	wpa_cli_run(out, sizeof(out), "ping", NULL, NULL, NULL, NULL);
	if (!strstr(out, "PONG")) {
		/* ★ 先把配置文件备好再启动。
		 *
		 *   update_config=1 这个开关是 wpa_supplicant【启动时】
		 *   从文件读进内存的。进程已经跑起来之后再改文件，
		 *   内存里还是旧值，save_config 照样返回 FAIL。
		 *   所以顺序必须是：ensure_wpa_conf() → 启动 → 连 → save。 */
		ensure_wpa_conf();

		char cmd[160];

		std::snprintf(cmd, sizeof(cmd),
			      "wpa_supplicant -B -i " IFNAME " -c %s 2>&1",
			      g_wpa_conf);
		run_cmd(cmd, out, sizeof(out));
		usleep(1200 * 1000);	/* 启动 + 初始化 SDIO 要 1 秒左右 */

		wpa_cli_run(out, sizeof(out), "ping", NULL, NULL, NULL, NULL);
		if (!strstr(out, "PONG")) {
			set_state(WIFI_CONN_FAILED, "启动服务失败",
				  "wpa_supplicant 起不来", NULL, NULL);
			goto done;
		}
	}

	/* 网卡必须 up 才能扫/连 */
	run_cmd("ifconfig " IFNAME " up 2>&1", NULL, 0);

	/* ---- 步骤 2：下发配置 ---- */
	set_state(WIFI_CONN_WORKING, "下发配置", NULL, ssid, "");

	/*
	 * ★ 先查有没有同 SSID 的网络。
	 *
	 *   直接 add_network 会让每次连接都新建一个块，而 save_config
	 *   会把它们全写进配置文件 —— 连三次就有三个一模一样的
	 *   network={...}。重启时 wpa_supplicant 面对多个同 SSID 的块
	 *   行为不确定（可能挑中 disabled 那个然后不连）。
	 *
	 *   已存在就复用 id：set_network 会覆盖旧 psk，所以改密码也走这条路。
	 *   复用时不删旧网络（那是用户之前存好的），只有新建的才在失败时清理。
	 */
	id = find_network_by_ssid(ssid, NULL);

	/*
	 * ★ 竞态：wpa_supplicant 刚启动时，配置还在异步加载，
	 *   list_networks 可能只返回表头（一行数据都没有）。
	 *
	 *   此时会误判"没有这个网络"→ 走 add_network 新建一个，
	 *   再加上配置文件里原有的那个，文件里就出现两个同 SSID 的块
	 *   （实测第一次连接确实多出一个 disabled=1 的）。
	 *
	 *   所以没找到就再等 1 秒重查一次 —— 只在"没找到"的路径上多花
	 *   这 1 秒，正常情况（已经连过）一次就命中。
	 */
	if (id < 0) {
		usleep(1000 * 1000);
		id = find_network_by_ssid(ssid, NULL);
	}

	if (id >= 0) {
		reused = true;
		printf("[WiFi] 已存在该网络 id=%d，复用（不重复建块）\n", id);
	} else {
		/* add_network 的输出只有一行数字 */
		wpa_cli_run(out, sizeof(out), "add_network", NULL, NULL, NULL, NULL);
		id = atoi(out);
		if (id < 0) {
			set_state(WIFI_CONN_FAILED, "下发配置失败",
				  "add_network 没返回 id", NULL, NULL);
			goto done;
		}
	}
	snprintf(id_str, sizeof(id_str), "%d", id);

	to_hex(ssid, hex, sizeof(hex));
	/* 打印下发的实际内容：SSID 为空是"看起来密码错、实际根本没网名"
	 * 这类问题的头号嫌疑，日志里一眼能看到 */
	printf("[WiFi] 下发配置: id=%d ssid=\"%s\" hex=%s psk_len=%d\n",
	       id, ssid, hex, psk ? (int)strlen(psk) : 0);

	if (hex[0] == '\0') {
		set_state(WIFI_CONN_FAILED, "SSID 为空",
			  "拿到的网络名是空串", NULL, NULL);
		goto done;
	}

	wpa_cli_run(out, sizeof(out), "set_network", id_str, "ssid", hex, NULL);
	if (!strstr(out, "OK")) {
		set_state(WIFI_CONN_FAILED, "SSID 无效",
			  "set_network ssid 失败", NULL, NULL);
		goto done;
	}

	if (psk && psk[0]) {
		/* ★★★ psk 值本身必须【带字面双引号】发给 wpa_supplicant。
		 *
		 * wpa_supplicant 区分两种 psk 写法：
		 *   psk="mypassword"   → 8~63 字符的密码短语（我们这种）
		 *   psk=<64 位 hex>    → 已经算好的 32 字节 PSK
		 * 不带引号时它按第二种解析，8 个字符直接判非法，
		 * 报"密码长度需 8-63 位"—— 这个报错误导性极强。
		 *
		 * 这里是 C 字符串拼接，双引号是【内容】的一部分：
		 *   psk_arg 的内容就是  "12345678"
		 * 然后交给 execvp 逐个 argv 传，不经过 shell，
		 * 所以这层引号不会被吃掉。
		 *
		 * 密码里若含 " 要写成 \"（wpa_supplicant 的转义约定），
		 * 含 \ 要写成 \\，否则会提前结束字符串。 */
		size_t i, o = 0;

		psk_arg[o++] = '"';
		for (i = 0; psk[i] && o + 3 < sizeof(psk_arg) - 1; i++) {
			if (psk[i] == '"' || psk[i] == '\\')
				psk_arg[o++] = '\\';
			psk_arg[o++] = psk[i];
		}
		psk_arg[o++] = '"';
		psk_arg[o] = '\0';

		/* 直接把 psk_arg 当 argv 传，不经 shell —— 这层引号就能原样送达 */
		wpa_cli_run(out, sizeof(out), "set_network", id_str, "psk",
			    psk_arg, NULL);

		if (!strstr(out, "OK")) {
			/* 把 wpa_cli 的原始回显也打出来，避免"密码无效"
			 * 这三个字掩盖真实原因（比如 CLI 语法错） */
			printf("[WiFi] set_network psk 失败，原始回显: %s\n", out);
			set_state(WIFI_CONN_FAILED, "密码无效",
				  "密码长度需 8-63 位", NULL, NULL);
			goto done;
		}
		printf("[WiFi] 密码已下发（%d 位）\n", (int)strlen(psk));
	} else {
		/* 开放网络 */
		wpa_cli_run(out, sizeof(out), "set_network", id_str,
			    "key_mgmt", "NONE", NULL);
	}

	/* ---- 步骤 3：切到新网络并等关联 ---- */
	set_state(WIFI_CONN_WORKING, "关联中", NULL, ssid, "");

	wpa_cli_run(NULL, 0, "enable_network", id_str, NULL, NULL, NULL);
	wpa_cli_run(NULL, 0, "select_network", id_str, NULL, NULL, NULL);

	while (waited_ms < ASSOC_TIMEOUT_S * 1000) {
		usleep(ASSOC_POLL_MS * 1000);
		waited_ms += ASSOC_POLL_MS;

		query_state(st, sizeof(st), NULL, 0);

		if (strcmp(st, "COMPLETED") == 0) {
			ok = true;
			break;
		}
		/* 这四个状态是致命错误，不用再等 */
		if (strcmp(st, "DISCONNECTED") == 0 && waited_ms > 3000) {
			/* 刚开始 DISCONNECTED 是正常的（正在尝试），
			 * 3 秒后还是 DISCONNECTED 基本就是密码错/信号太差 */
			continue;
		}
		if (strstr(st, "4WAY_HANDSHAKE") || strcmp(st, "SCANNING") == 0)
			continue;

		/* 每 2 秒更新一次 stage 里的秒数，让用户知道在等 */
		if (waited_ms % 2000 == 0) {
			char tmp[24];

			snprintf(tmp, sizeof(tmp), "关联中 %ds", waited_ms / 1000);
			set_state(WIFI_CONN_WORKING, tmp, NULL, NULL, NULL);
		}
	}

	if (!ok) {
		/* 把最后看到的 wpa_state 一起报出来 ——
		 * "关联超时"这四个字信息量太低，看不到到底卡在
		 * 扫描、四次握手、还是认证被拒。 */
		char msg[64];

		snprintf(msg, sizeof(msg), "密码错误或信号弱（最后状态 %s）",
			 st[0] ? st : "无响应");
		set_state(WIFI_CONN_FAILED, "关联超时", msg, NULL, NULL);
		goto done;
	}

	/* ---- 步骤 4：DHCP 拿 IP ---- */
	set_state(WIFI_CONN_WORKING, "获取IP", NULL, ssid, "");

	/* ★ 见 ensure_usable_ip() 的注释：只看"有没有 IP"是不够的，
	 *   换热点后旧地址还挂在网卡上，会让人误以为已经拿到 IP 而跳过 DHCP，
	 *   结果就是"连着但 DNS 错"。这里统一走判定 + 按需 DHCP。 */
	if (!ensure_usable_ip(ip, sizeof(ip)))
		printf("[WiFi] DHCP 后仍没有可用 IP\n");

	if (ip[0] == '\0') {
		set_state(WIFI_CONN_FAILED, "DHCP 失败",
			  "关联成功但没拿到 IP", NULL, NULL);
		goto done;
	}

	/* DHCP 刚把本网卡的 DNS 追加到 resolv.conf 末尾，
	 * 重排一下把本地 DNS 提到最前（否则解析要等公网 DNS 超时）。 */
	reorder_resolv(IFNAME);
	ensure_dns();		/* resolv.conf 在 tmpfs 上，重启是空的，兜底补 */

	set_state(WIFI_CONN_CONNECTED, "已连接", NULL, ssid, ip);
	printf("[WiFi] 连接成功: %s  IP=%s\n", ssid, ip);

	/* ★ 落盘 —— 这一步以前【完全没有】，所以"保存的 WiFi 重启就没了"。
	 *
	 *   放在连接成功之后而不是下发配置之后：
	 *   密码错的网络不该被记住。否则下次开机会自动拿错密码反复
	 *   重试（每次 25 秒超时），用户还得先手动删掉才能连对的。 */
	if (!save_wpa_config()) {
		/* 不阻断：本次已经能上网了，只是重启后要重输 */
		set_state(WIFI_CONN_CONNECTED, "已连接（未保存）", NULL,
			  ssid, ip);
	}

	/* ★ 网络真正可用的这一刻才校时。
	 *
	 * 系统的 /etc/init.d/S49ntp 在开机早期就拉起了 ntpd，
	 * 但那时 WiFi 还没连（WiFi 驱动要等 S99start_lvgl 才装），
	 * 上游服务器一个都解析不了，所以时间一直停在上电值。
	 *
	 * 放在这里而不是"开机后等 N 秒"：等多久都可能不够，
	 * 而 DHCP 成功 + 路由 + ARP 都验过才是真的能出网。
	 * timesync_start() 是异步的并且 5 分钟内幂等，
	 * 所以网络抖动重连不会反复改系统时间。 */
	timesync_start();

done:
	/* 失败的时候把刚才建的网络删掉，别在 wpa_supplicant 里
	 * 留一堆半成品配置（下次 list_networks 会很难看） */
	if (id >= 0) {
		int st_state;

		g_lock.lock();
		st_state = g_ci.state;
		g_lock.unlock();

		/* 只删本次新建的。复用的是用户之前保存好的网络，
		 * 删掉等于把人家能用的配置也弄丢了。 */
		if (st_state != WIFI_CONN_CONNECTED && !reused) {
			wpa_cli_run(NULL, 0, "remove_network", id_str,
				    NULL, NULL, NULL);
		}
	}

	{
		std::lock_guard<std::mutex> lk(g_lock);

		g_busy = false;
	}
	/* ssid / psk 是 std::string，函数返回自动释放 */
}

/* ==================================================================
 * 持久化：让"保存的 WiFi"真的活过重启
 *
 * 【背景 —— 这是一个完全静默的 bug】
 *   用户："保存的 wifi 下次重启后又没有了。"
 *
 *   代码里从头到尾【没有调用过 save_config】。而 wpa_cli 的
 *   add_network / set_network 只改 wpa_supplicant 【内存】里的配置，
 *   一个字节都不落盘。重启后 wpa_supplicant 重新读配置文件，
 *   里面根本没有这个 network → 一切归零。
 *
 *   为什么一直没被发现：
 *     wifi_conn_is_saved() 查的是 `list_networks`，那也是【内存】列表。
 *     所以在同一次开机里它永远返回"已保存"，UI 正常显示、还能自动重连，
 *     看不出任何异常。问题只在下次重启才暴露 —— 而那时人会以为是
 *     "WiFi 坏了"或"密码没记住"，很难联想到"压根没写盘"。
 *
 * 【修法两件事】
 *   ① 配置文件里必须有 update_config=1，否则 save_config 直接返回 FAIL。
 *      这个开关是 wpa_supplicant 【启动时】读进内存的，进程跑起来之后
 *      再改文件没用 —— 所以必须在本函数里、启动之前就备好。
 *   ② 连接成功后显式调一次 save_config 落盘。
 * ================================================================== */

/* 读整个文件到字符串；失败返回空串 */
static std::string read_file(const char *path)
{
	std::string out;
	res::File f(std::fopen(path, "r"));

	if (!f)
		return out;

	char buf[512];
	size_t n;

	while ((n = std::fread(buf, 1, sizeof(buf), f.get())) > 0)
		out.append(buf, n);
	return out;
}

/**
 * 确保配置就绪：存在、有 ctrl_interface、有 update_config=1。
 *
 * 必须在启动 wpa_supplicant 之前调用。
 * @return 0 成功（g_wpa_conf 指向可用路径）；-1 全失败（WiFi 仍可连，但不持久）
 */
static int ensure_wpa_conf(void)
{
	std::string content;

	/* 1. 优先用已有的 /userdata 那份；没有就从出厂 /etc 那份复制过来 */
	content = read_file(WPA_CONF);
	if (content.empty())
		content = read_file(WPA_CONF_ORIGIN);

	/* 2. 逐行重建：update_config 统一成 1，缺 ctrl_interface 就补上。
	 *
	 * 为什么不直接 append 一行 update_config=1：
	 *   如果原文件里已经有 update_config=0，追加一行 1 之后到底哪个生效
	 *   取决于 wpa_supplicant 解析时的覆盖规则 —— 这种"依赖实现细节"的
	 *   写法不能碰。统一重建最干净。
	 */
	std::string out;
	bool has_update = false, has_ctrl = false;
	size_t pos = 0;

	while (pos < content.size()) {
		size_t eol = content.find('\n', pos);
		bool last = (eol == std::string::npos);
		std::string line = last ? content.substr(pos)
					: content.substr(pos, eol - pos);

		/* 去掉首尾空白后再判断，避免 "  update_config = 1" 这种漏判 */
		size_t a = line.find_first_not_of(" \t\r");
		size_t b = line.find_last_not_of(" \t\r");
		std::string t = (a == std::string::npos) ? std::string()
					: line.substr(a, b - a + 1);

		if (t.rfind("update_config", 0) == 0) {
			out += "update_config=1\n";
			has_update = true;
		} else if (t.rfind("ctrl_interface", 0) == 0) {
			/* wpa_cli 靠这个 socket 和守护通信，路径必须一致 */
			out += "ctrl_interface=/var/run/wpa_supplicant\n";
			has_ctrl = true;
		} else {
			out += line;
			out += '\n';
		}

		if (last)
			break;
		pos = eol + 1;
	}

	if (!has_update)
		out += "update_config=1\n";
	if (!has_ctrl)
		out = "ctrl_interface=/var/run/wpa_supplicant\n" + out;

	/* 3. 先写临时文件再 rename。
	 *
	 *   ★ 不能直接 fopen(path,"w")：那样会先把原文件截断，
	 *     万一后面写失败（空间不足），原有配置就彻底没了
	 *     —— 那比"没保存"更糟，等于把之前能用的网络也删了。
	 */
	const char *cands[2] = { WPA_CONF, WPA_CONF_ORIGIN };

	for (int i = 0; i < 2; i++) {
		std::string tmp = std::string(cands[i]) + ".tmp";
		res::File f(std::fopen(tmp.c_str(), "w"));

		if (!f) {
			printf("[WiFi] 无法创建 %s: %s\n", tmp.c_str(),
			       std::strerror(errno));
			continue;
		}

		size_t n = std::fwrite(out.data(), 1, out.size(), f.get());

		if (n != out.size()) {
			printf("[WiFi] 写 %s 只写了 %zu/%zu 字节（空间不足？）\n",
			       tmp.c_str(), n, out.size());
			f.reset();
			::unlink(tmp.c_str());
			continue;
		}
		f.reset();		/* 必须先关，否则 rename 后数据可能没落盘 */

		if (::rename(tmp.c_str(), cands[i]) != 0) {
			printf("[WiFi] rename %s 失败: %s\n", tmp.c_str(),
			       std::strerror(errno));
			::unlink(tmp.c_str());
			continue;
		}

		std::snprintf(g_wpa_conf, sizeof(g_wpa_conf), "%s", cands[i]);
		return 0;
	}

	/* 两个位置都写不进去：不阻断连接，只是重启后要重输密码 */
	printf("[WiFi] ★ 配置无法持久化，本次连接可用但重启后会丢失\n");
	return -1;
}

/**
 * 把内存里的 network 配置落盘。
 *
 * 只在【连接成功之后】调用 —— 密码错的网络不该被记住，否则下次开机
 * wpa_supplicant 会拿着错密码反复重试，用户还得手动删。
 */
static bool save_wpa_config(void)
{
	char out[512];

	wpa_cli_run(out, sizeof(out), "save_config", NULL, NULL, NULL, NULL);

	if (std::strstr(out, "OK")) {
		printf("[WiFi] 配置已保存到 %s（重启后会自动使用）\n", g_wpa_conf);
		return true;
	}

	printf("[WiFi] ★ save_config 失败，原始回显: %s\n", out);
	printf("[WiFi]   常见原因：① 配置缺 update_config=1 "
	       "② 分区只读或空间不足\n");
	return false;
}

/* 解码 iw/wpa_cli 输出里的 \xNN 转义（中文 SSID） */
static void decode_escaped(const char *in, char *out, size_t outsz)
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

/**
 * 启动时检测"其实已经连着"的情况
 *
 * 为什么需要：wpa_supplicant 是独立进程，可能在我们这个程序
 * 启动之前就已经连上了（比如程序崩溃重启、或者别的地方手动连的）。
 * 这时如果 init 无脑把状态置成 IDLE，界面显示"未连接"，
 * 但实际能上网 —— 用户会一头雾水。
 *
 * 所以启动时同步查一次。这个 popen 有几十毫秒开销，
 * 只在初始化时做一次，可以接受。
 */
static void load_current_conn(void)
{
	char out[1024];
	const char *p;
	char ssid[128], ip[16];

	wpa_cli_run(out, sizeof(out), "status", NULL, NULL, NULL, NULL);

	if (!strstr(out, "wpa_state=COMPLETED"))
		return;

	ssid[0] = '\0';
	p = strstr(out, "\nssid=");
	if (!p)
		p = strstr(out, "ssid=");	/* 万一 ssid 是第一行 */
	if (p) {
		const char *q = strchr(p, '=');
		size_t i = 0;

		if (q) {
			q++;
			/* 取到行尾（\n 或串尾） */
			while (*q && *q != '\n' && i + 1 < sizeof(ssid))
				ssid[i++] = *q++;
			ssid[i] = '\0';
		}
	}
	{
		char dec[WIFI_SSID_MAX * 4];

		decode_escaped(ssid, dec, sizeof(dec));
		snprintf(ssid, sizeof(ssid), "%s", dec);
	}

	ip[0] = '\0';
	run_cmd("ifconfig " IFNAME " 2>&1", out, sizeof(out));
	parse_ip(out, ip, sizeof(ip));

	/* 光有 IP 不算"能用"：还要有默认路由指向它，且网关 ARP 已解析。
	 * 否则会出现"界面显示已连接、实际什么都 ping 不通"的假连通
	 * （旧网段的地址残留在网卡上时就是这种情况）。 */
	if (ip[0]) {
		char gw[32] = "";
		bool gw_ok = get_default_gw(IFNAME, gw, sizeof(gw));

		if (!gw_ok) {
			printf("[WiFi] 启动检测: 有 IP %s 但无默认路由，视为未连接\n", ip);
			return;
		}

		g_ci.state = WIFI_CONN_CONNECTED;
		snprintf(g_ci.stage, sizeof(g_ci.stage), "已连接");
		snprintf(g_ci.ssid, sizeof(g_ci.ssid), "%s", ssid);
		snprintf(g_ci.ip, sizeof(g_ci.ip), "%s", ip);
		g_ci.seq++;

		/* 启动时也顺手修一次 DNS 顺序（上次会话可能留下的顺序问题） */
		reorder_resolv(IFNAME);

		printf("[WiFi] 启动检测: 已连接 %s IP=%s 网关=%s\n", ssid, ip, gw);

		/* 程序启动时网络就已经是通的（上次连的还活着），
		 * 这时也要校时 —— 否则只有"本次启动里主动连一次网"
		 * 才会校时，而开机自启的场景恰恰是启动即已连接。 */
		timesync_start();
	}
}

extern "C" void wifi_conn_init(void)
{
	memset(&g_ci, 0, sizeof(g_ci));
	g_ci.state = WIFI_CONN_IDLE;
	snprintf(g_ci.stage, sizeof(g_ci.stage), "未连接");
	g_busy = false;

	/* 查一下是不是启动前就已经连着了（见上面注释） */
	load_current_conn();
}

extern "C" void wifi_conn_start(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return;

	{
		std::lock_guard<std::mutex> lk(g_lock);

		if (g_busy)
			return;         /* 已有连接任务在跑 */
		g_busy = true;
	}

	/*
	 * ★ 原来这里要 malloc 一个 char*[2] 再 strdup 两个字符串，
	 *   失败时把三块逐个 free；线程里再 free 三次。
	 *   一共 5 个分配点、3 个释放点、两条失败路径。
	 *
	 *   现在：lambda 按值捕获两个 std::string，拷贝进线程栈帧，
	 *   线程结束自动释放。分配/释放点各自归零。
	 *
	 *   std::thread 必须 join 或 detach 才能析构，这里 detach
	 *   （"发出去就不管"的一次性任务）。构造失败会抛 system_error，
	 *   所以包一层 try —— 板子只有 54MB，线程创建失败是真会发生的。
	 */
	try {
		std::thread th([s = std::string(ssid), p = std::string(psk ? psk : "")]() {
			conn_thread(s, p);
		});

		th.detach();
	} catch (...) {
		std::lock_guard<std::mutex> lk(g_lock);

		g_busy = false;
		printf("[WiFi] 创建连接线程失败（内存/线程数不足）\n");
	}
}

extern "C" void wifi_conn_disconnect(void)
{
	char out[256];

	wpa_cli_run(out, sizeof(out), "disconnect", NULL, NULL, NULL, NULL);
	/* 直接把地址抹掉：比 udhcpc -R 更快也不会因为"服务器不可达"而失效
	 * （-R 要先 DISCOVER 成功才走到释放，断网时就白等十几秒） */
	run_cmd("ifconfig " IFNAME " 0.0.0.0 2>&1", NULL, 0);

	set_state(WIFI_CONN_IDLE, "未连接", NULL, NULL, "");
	printf("[WiFi] 已断开\n");
}

extern "C" int wifi_conn_get(struct wifi_conn_info *out)
{
	int seq;

	g_lock.lock();
	memcpy(out, &g_ci, sizeof(*out));
	seq = g_ci.seq;
	g_lock.unlock();

	return seq;
}

/**
 * 查某个 SSID 是否已在 wpa_supplicant 里配过
 *
 * `wpa_cli list_networks` 输出形如：
 *   network id / ssid / bssid / flags
 *   0  \xe5\xb0\x8f\xe6\xa1\x83-2.4G  any  [CURRENT]
 * 注意 SSID 是 hex 转义的，要解码后再比。
 */
extern "C" int wifi_conn_is_saved(const char *ssid)
{
	char out[2048];
	char *line, *next;
	bool found = false;

	if (!ssid || !ssid[0])
		return 0;

	wpa_cli_run(out, sizeof(out), "list_networks", NULL, NULL, NULL, NULL);

	for (line = out; line && *line; line = next) {
		char decoded[WIFI_SSID_MAX * 4];
		char *p, *tab;
		size_t oi = 0;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		/* 跳过表头 */
		if (strncmp(line, "network id", 10) == 0)
			continue;

		/* 第 1 个 tab 之后是 ssid */
		tab = strchr(line, '\t');
		if (!tab)
			continue;
		p = tab + 1;

		/* 解码 \xNN 形式的 SSID */
		while (*p && *p != '\t' && oi + 1 < sizeof(decoded)) {
			if (p[0] == '\\' && p[1] == 'x') {
				char hex[3] = { p[2], p[3], 0 };

				decoded[oi++] = (char)strtol(hex, NULL, 16);
				p += 4;
			} else {
				decoded[oi++] = *p++;
			}
		}
		decoded[oi] = '\0';

		if (strcmp(decoded, ssid) == 0) {
			found = true;
			break;
		}
	}

	return found;
}


static void saved_conn_thread(std::string ssid_holder)
{
	const char *ssid = ssid_holder.c_str();
	char out[1024];
	char st[32];
	char idstr2[16];
	int id, waited = 0;

	/* select 已保存的网络：它的凭据 wpa_supplicant 已经知道，
	 * 一条 select_network 就行，不用重下发 ssid/psk。 */
	wpa_cli_run(out, sizeof(out), "ping", NULL, NULL, NULL, NULL);
	if (!strstr(out, "PONG")) {
		/* 和 conn_thread 一样：先备好配置（update_config=1 必须在
		 * 启动前就在文件里），再用持久路径启动。 */
		ensure_wpa_conf();

		char cmd[160];

		std::snprintf(cmd, sizeof(cmd),
			      "wpa_supplicant -B -i " IFNAME " -c %s 2>&1",
			      g_wpa_conf);
		run_cmd(cmd, out, sizeof(out));
		usleep(1200 * 1000);
	}

	set_state(WIFI_CONN_WORKING, "关联中", NULL, ssid, "");

	id = -1;
	{
		/* 找到该 SSID 对应的网络 id。为省事直接遍历一遍
		 * list_networks，按 SSID 匹配（解码逻辑同上）。 */
		char lst[2048];
		char *line, *next;

		wpa_cli_run(lst, sizeof(lst), "list_networks", NULL, NULL, NULL, NULL);

		for (line = lst; line && *line; line = next) {
			char decoded[WIFI_SSID_MAX * 4];
			char *p, *tab;
			size_t oi = 0;

			next = strchr(line, '\n');
			if (next)
				*next++ = '\0';
			if (strncmp(line, "network id", 10) == 0)
				continue;

			tab = strchr(line, '\t');
			if (!tab)
				continue;

			int nid = atoi(line);

			p = tab + 1;
			while (*p && *p != '\t' && oi + 1 < sizeof(decoded)) {
				if (p[0] == '\\' && p[1] == 'x') {
					char hex[3] = { p[2], p[3], 0 };

					decoded[oi++] = (char)strtol(hex, NULL, 16);
					p += 4;
				} else {
					decoded[oi++] = *p++;
				}
			}
			decoded[oi] = '\0';

			if (strcmp(decoded, ssid) == 0) {
				id = nid;
				break;
			}
		}
	}

	if (id < 0) {
		set_state(WIFI_CONN_FAILED, "无保存的凭据",
			  "这个网络没连过，需要输密码", NULL, NULL);
		goto out;
	}

	snprintf(idstr2, sizeof(idstr2), "%d", id);
	wpa_cli_run(NULL, 0, "select_network", idstr2, NULL, NULL, NULL);

	while (waited < ASSOC_TIMEOUT_S * 1000) {
		usleep(ASSOC_POLL_MS * 1000);
		waited += ASSOC_POLL_MS;
		query_state(st, sizeof(st), NULL, 0);
		if (strcmp(st, "COMPLETED") == 0)
			break;
	}

	if (strcmp(st, "COMPLETED") != 0) {
		set_state(WIFI_CONN_FAILED, "关联超时", "信号弱或密码变了",
			  NULL, NULL);
		goto out;
	}

	set_state(WIFI_CONN_WORKING, "获取IP", NULL, ssid, "");

	{
		char ip[16];

		/* ★ 原来这里是 `if (ip[0] == '\0') 才 DHCP` —— 切热点时
		 *   旧网络的地址还挂在 wlan0 上，ip 非空 → 跳过 DHCP →
		 *   没有新网关、resolv.conf 里没有新 DNS。
		 *   表现就是"WiFi 连上了但 DNS 错误"。
		 *   现在统一用 ensure_usable_ip() 判定。 */
		if (ensure_usable_ip(ip, sizeof(ip))) {
			ensure_dns();
			reorder_resolv(IFNAME);
			set_state(WIFI_CONN_CONNECTED, "已连接", NULL, ssid, ip);
			timesync_start();	/* 免密重连成功，同样校时 */
		} else {
			set_state(WIFI_CONN_FAILED, "DHCP 失败",
				  "关联成功但没拿到可用 IP", NULL, NULL);
		}
	}

out:
	{
		std::lock_guard<std::mutex> lk(g_lock);

		g_busy = false;
	}
	/* ssid 是 std::string（按值传入），函数返回自动释放 */
}

extern "C" int wifi_conn_connect_saved(const char *ssid)
{
	if (!ssid || !ssid[0])
		return 0;
	if (!wifi_conn_is_saved(ssid))
		return 0;

	{
		std::lock_guard<std::mutex> lk(g_lock);

		if (g_busy)
			return 0;
		g_busy = true;
	}

	/*
	 * ★ 原来有一段很别扭的逻辑：为了把 ssid 传给线程，
	 *   先 strdup 到全局 g_saved_ssid，再把它当线程参数传进去，
	 *   线程里 free —— 同一个指针既是全局变量又是线程参数，
	 *   谁负责释放完全靠约定。而且 g_saved_ssid 在 free 之后
	 *   还留着野指针，下次有人用到就是 UB。
	 *
	 *   现在 g_saved_ssid 这个全局变量【直接删掉】，
	 *   ssid 靠 lambda 值捕获传进线程，所有权清晰。
	 */
	try {
		std::thread th([s = std::string(ssid)]() {
			saved_conn_thread(s);
		});

		th.detach();
		return 1;
	} catch (...) {
		std::lock_guard<std::mutex> lk(g_lock);

		g_busy = false;
		return 0;
	}
}
