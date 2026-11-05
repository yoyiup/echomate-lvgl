/**
 * block_test.c —— 验证"同步扫描会卡住 LVGL 主循环"的对照实验
 *
 * 用途：说明为什么 wifi_scan.c 必须用后台线程。
 * 在同一块板上分别测：
 *   A. 直接 popen("iw ... scan") 同步读      ← 这段若放进主循环就冻界面
 *   B. 后台线程跑，主循环只管取结果
 *
 * 编译（宿主交叉编译）：
 *   arm-rockchip830-linux-uclibcgnueabihf-gcc -O2 -o block_test block_test.c
 *   scp block_test root@板子:/tmp/ && ssh 板子 /tmp/block_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void)
{
	double t0, t1;
	FILE *fp;
	char buf[256];
	int n = 0, i;

	printf("=== A. 同步 popen 扫描 ===\n");
	for (i = 0; i < 3; i++) {
		t0 = now_ms();
		fp = popen("iw dev wlan0 scan 2>/dev/null", "r");
		if (!fp) {
			printf("  popen 失败\n");
			return 1;
		}
		n = 0;
		while (fgets(buf, sizeof(buf), fp))
			n++;
		pclose(fp);
		t1 = now_ms();
		printf("  第 %d 次: %.0f ms, %d 行输出\n", i + 1, t1 - t0, n);
	}

	printf("\n→ 如果这段代码写在 LVGL 主循环里，界面就会卡住这么久：\n");
	printf("   不收触摸、不重绘、动画停住。用户会以为程序死了。\n");
	printf("   所以扫描必须丢到后台线程（见 src/wifi/wifi_scan.c）。\n");
	return 0;
}
