/**
 * wpa_psk_test.c —— 验证"ps_sk 下发方式"的对照实验
 *
 * 这个程序解决一个具体问题：为什么用 popen 传密码会失败。
 *
 * 板子上交叉编译：
 *   arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o wpa_psk_test wpa_psk_test.c
 *   scp wpa_psk_test root@板子:/tmp/ && ssh 板子 /tmp/wpa_psk_test
 *
 * 预期结果：
 *   A（popen，单层引号）  → FAIL   ← shell 把引号吃掉，wpa 按 hex PSK 解析
 *   B（popen，双层引号）  → OK     ← 但密码含 $ ` 等仍会被 shell 解释
 *   C（execvp，argv 直传）→ OK     ← 正解，任何字符都安全
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>

#define IFNAME "wlan0"
#define TEST_PSK "12345678"	/* 8 位，合法密码短语 */

/* 经 shell 执行（popen） */
static int via_shell(const char *cmd, char *out, size_t outsz)
{
	FILE *fp = popen(cmd, "r");
	size_t used = 0, n;

	if (!fp)
		return -1;
	while (used + 1 < outsz &&
	       (n = fread(out + used, 1, outsz - used - 1, fp)) > 0)
		used += n;
	out[used] = '\0';
	return pclose(fp) >> 8;
}

/* 不经 shell 执行（execvp），argv 逐个传 */
static int via_exec(char *const argv[], char *out, size_t outsz)
{
	int pfd[2], status = -1;
	pid_t pid;
	ssize_t n;
	size_t used = 0;

	if (pipe(pfd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		close(pfd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	if (out && outsz) {
		while (used + 1 < outsz &&
		       (n = read(pfd[0], out + used, outsz - used - 1)) > 0)
			used += n;
		out[used] = '\0';
	} else {
		char sink[128];

		while (read(pfd[0], sink, sizeof(sink)) > 0)
			;
	}
	close(pfd[0]);
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 建一个临时网络，返回 id */
static int add_net(void)
{
	char out[128];

	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "add_network", NULL }, out, sizeof(out));
	return atoi(out);
}

int main(void)
{
	char out[512], cmd[256], idstr[16], pskq[64];
	int id;
	bool ok;

	printf("=== 前提：wpa_supplicant 在跑 ===\n");
	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "ping", NULL }, out, sizeof(out));
	if (!strstr(out, "PONG")) {
		printf("wpa_supplicant 没在跑，先启动\n");
		system("wpa_supplicant -B -i " IFNAME
		       " -c /etc/wpa_supplicant.conf");
		sleep(2);
	}
	printf("PONG ok\n\n");

	/* ---------- A：popen + 单层引号 ---------- */
	id = add_net();
	snprintf(idstr, sizeof(idstr), "%d", id);
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/wpa_cli -i %s set_network %s psk \"%s\" 2>&1",
		 IFNAME, idstr, TEST_PSK);
	via_shell(cmd, out, sizeof(out));
	ok = strstr(out, "OK") != NULL;
	printf("A  popen 单层引号  -> %s   (回显: %s)",
	       ok ? "OK" : "FAIL", out);
	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "remove_network", idstr, NULL }, NULL, 0);

	/* ---------- B：popen + 双层引号 ---------- */
	id = add_net();
	snprintf(idstr, sizeof(idstr), "%d", id);
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/wpa_cli -i %s set_network %s psk '\\\"%s\\\"' 2>&1",
		 IFNAME, idstr, TEST_PSK);
	via_shell(cmd, out, sizeof(out));
	ok = strstr(out, "OK") != NULL;
	printf("B  popen 双层引号  -> %s   (回显: %s)",
	       ok ? "OK" : "FAIL", out);
	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "remove_network", idstr, NULL }, NULL, 0);

	/* ---------- C：execvp + argv 直传 ---------- */
	id = add_net();
	snprintf(idstr, sizeof(idstr), "%d", id);
	snprintf(pskq, sizeof(pskq), "\"%s\"", TEST_PSK);	/* 带字面引号 */
	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "set_network", idstr, "psk", pskq, NULL },
		 out, sizeof(out));
	ok = strstr(out, "OK") != NULL;
	printf("C  execvp argv     -> %s   (回显: %s)",
	       ok ? "OK" : "FAIL", out);
	via_exec((char *const[]){ "/usr/sbin/wpa_cli", "-i", IFNAME,
				  "remove_network", idstr, NULL }, NULL, 0);

	printf("\n结论：A 失败是因为 shell 吃掉了引号，wpa 把裸字符串\n"
	       "      当成 64 位 hex PSK 解析 → 长度不符 → 报"
	       "\"密码长度需 8-63 位\"。\n"
	       "      **用户密码完全正确也会这样报错。**\n"
	       "      正解是 C：fork+execvp 逐个 argv 传，不经 shell。\n");
	return 0;
}
