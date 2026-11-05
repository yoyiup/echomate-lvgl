/**
 * weather.c —— 拉取并解析天气 JSON
 *
 * 解析策略：这个接口的响应格式固定（约 4KB），
 * 每个字段都是 "key":"value" 或 "key":number 的形式，
 * 所以用"找 `"key":` 再跳过引号取值"就够了，不需要完整 JSON 解析器。
 *
 * 要注意一个陷阱：天气接口里 "type"/"high"/"low"/"notice" 这些 key
 * 在【今天】和【未来几天】的 forecast 里都会出现。
 * 我们只想要今天的，所以必须【限定在 data 对象内】搜索 ——
 * 简单做法是：先找到 `"forecast":[` 的位置，只在它之前搜。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdbool>
#include <unistd.h>
#include <time.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "weather.h"
#include "resources.hpp"     /* res::Pipe —— popen 的 RAII 封装 */

static struct weather_info g_w;
static std::mutex g_lock;
static bool g_busy;
static int g_seq;		/* 每次拉取完成 +1，UI 靠它判断要不要重画 */

#define API_URL "http://t.weather.itboy.net/api/weather/city/"

/**
 * 从 JSON 里取一个字符串字段的值
 *
 * hay 是搜索范围（可以是整段，也可以是限定过的子串），
 * key 形如 "wendu"，会自动补上 `"` 和 `":"` 去匹配。
 * 返回值写进 out。
 */
static bool jstr(const char *hay, const char *end, const char *key,
		 char *out, size_t outsz)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	p = strstr(hay, pat);
	if (!p || (end && p > end))
		return false;
	p += strlen(pat);

	const char *q = strchr(p, '"');
	if (!q || (end && q > end))
		return false;

	size_t n = (size_t)(q - p);

	if (n >= outsz)
		n = outsz - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return true;
}

/* 取数字字段（可能是 24.0 这种浮点，只取整数部分） */
static bool jnum(const char *hay, const char *end, const char *key, int *out)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(hay, pat);
	if (!p || (end && p > end))
		return false;
	p += strlen(pat);

	*out = atoi(p);
	return true;
}

/* 从 "高温 30℃" 里抠出 30 */
static int parse_temp(const char *s)
{
	const char *p = s;

	while (*p && (*p < '0' || *p > '9')) {
		/* 负温度："低温 -3℃" */
		if (*p == '-' && p[1] >= '0' && p[1] <= '9')
			break;
		p++;
	}
	return atoi(p);
}

/*
 * 拉取线程。
 *
 * ★ 相比原来的 C 版，这里只用 RAII 管资源，解析逻辑一行没动：
 *
 *   原来要手工配对的资源有三样：
 *     popen/fp        → 3 条出口路径，2 处 pclose  + 1 处漏（popen 失败时）
 *     malloc(json)    → free 放在 store 标签后面，靠 goto 保证
 *     strdup(code)    → 线程参数，free 在末尾
 *   这三样任何一处漏，跑久了就是内存/句柄泄漏，而现象是"天气偶尔不刷新"，
 *   完全联想不到是资源问题。
 *
 *   现在：
 *     res::Pipe  → 析构自动 pclose（并且 waitpid，不留僵尸进程）
 *     std::vector→ 自动释放
 *     std::string→ 值捕获进线程，不需要手工 strdup/free
 *
 *   注意 wget 这里仍走 shell（res::run_shell 的路径），因为命令里有
 *   引号和 2>&1 重定向，需要 shell 解析。citycode 是本地写死的城市码，
 *   不含用户输入，没有注入风险。
 */
static void fetch_thread(std::string code)
{
	char cmd[256];
	const size_t cap = 8192;
	size_t used = 0;
	struct weather_info w;

	/*
	 * ★ 缓冲区必须声明在函数最前面，不能放在 goto 之后。
	 *
	 *   C++ 规定：goto 不允许【跳过】带非平凡析构的变量初始化
	 *   （否则跳过去之后变量既没构造也不会析构，语义不可定义）。
	 *   编译器报：jump to label 'store' crosses initialization of
	 *            'std::vector<char> jbuf'
	 *
	 *   所以即使逻辑上"用到它之前再声明"更自然，也必须提上来。
	 *   这是 C++ 和 C 的一个实质差异：C 里 goto 可以随便跳过变量。
	 */
	std::vector<char> jbuf(cap);
	char *json = jbuf.data();
	size_t n;

	std::memset(&w, 0, sizeof(w));
	w.aqi = -1;

	std::snprintf(cmd, sizeof(cmd),
		      "wget -q -T 10 -t 1 -O - \"" API_URL "%s\" 2>&1",
		      code.c_str());

	res::Pipe fp;

	if (!fp.open(cmd)) {
		std::snprintf(w.err, sizeof(w.err), "popen 失败");
		goto store;
	}

	/*
	 * ★ 缓冲提到函数作用域，并且【变量名仍叫 json】。
	 *   这样下面那 150 行解析代码一个字都不用改 ——
	 *   转换的风险全在"顺手改逻辑"，只换资源管理才不会引入回归。
	 *   真正的释放由 jbuf（vector）在函数返回时自动完成。
	 */
	while (used + 1 < cap &&
	       (n = std::fread(json + used, 1, cap - used - 1, fp.get())) > 0)
		used += n;
	json[used] = '\0';

	/* ★ 先把管道关掉（等 wget 真正退出，拿到真实退出码），再解析 */
	fp.close();

	if (used < 32) {
		snprintf(w.err, sizeof(w.err), "响应为空（网络不通？）");
		goto store;
	}
	/* ★ wget 的 stderr 被 2>&1 混进来了，所以"网络不通"和"接口返回异常"
	 *   在这里长得一模一样（都是没有 "status":200）。以前统一报
	 *   "接口返回异常"，结果开机时 WiFi 还没起来，屏幕上写着"接口返回异常"，
	 *   把人往错误方向带。这里按 wget 自己的报错区分开。 */
	if (strstr(json, "wget:") || strstr(json, "can't connect") ||
	    strstr(json, "unable to resolve") || strstr(json, "bad address")) {
		snprintf(w.err, sizeof(w.err), "网络未就绪");
		goto store;
	}
	if (!strstr(json, "\"status\":200") && !strstr(json, "success")) {
		snprintf(w.err, sizeof(w.err), "接口返回异常");
		goto store;
	}

	/* ---- 关键：这个接口的字段分两处，位置完全不同 ----
	 *
	 * data 顶层（forecast 数组【之前】）只有这几项：
	 *     shidu / pm25 / pm10 / quality / wendu / ganmao
	 *
	 * 而 type / high / low / notice / date 全在
	 * forecast 数组的【第一个元素】里（= 今天）。
	 *
	 * 我第一版把搜索范围限定成"forecast 之前"，
	 * 结果 type 拿到空串、high/low 拿到 0 ——
	 * 因为那些字段本来就不在那儿。
	 *
	 * 所以正确做法是分两段：
	 *   段 A = forecast 之前   → 取 温/湿/空气质量
	 *   段 B = forecast 之后   → 取 今天(第一个)的 天气/温度区间
	 */
	{
		const char *fstart = strstr(json, "\"forecast\":[");
		const char *day_end;
		char tmp[64];

		/* city 在 cityInfo 里，位置靠前 */
		if (!jstr(json, fstart, "city", tmp, sizeof(tmp)))
			snprintf(tmp, sizeof(tmp), "未知");
		snprintf(w.city, sizeof(w.city), "%s", tmp);

		/* ---------- 段 A：data 顶层 ---------- */
		if (jstr(json, fstart, "wendu", tmp, sizeof(tmp)))
			w.temp = atoi(tmp);
		else
			w.temp = -99;

		if (jstr(json, fstart, "shidu", tmp, sizeof(tmp)))
			w.humidity = atoi(tmp);	/* "68%" → 68 */

		jstr(json, fstart, "quality", w.quality, sizeof(w.quality));
		jnum(json, fstart, "pm25", &w.aqi);
		jstr(json, fstart, "ganmao", w.notice, sizeof(w.notice));

		/* ---------- 段 B：forecast 里的第一个元素（今天）----------
		 * day_end 限定到第二个元素之前（下一个 "date" 出现处），
		 * 防止取到明天的数据。简单可靠的界标是 forecast 里
		 * 每个元素都以 "date":"...","high":... 开头，
		 * 所以从 fstart 之后找第二个 "date" 作为结束。 */
		day_end = NULL;
		if (fstart) {
			const char *p1 = strstr(fstart, "\"date\":");
			const char *p2 = p1 ? strstr(p1 + 1, "\"date\":") : NULL;

			day_end = p2;
			if (!day_end)
				day_end = json + strlen(json);	/* 没有第二天就用末尾 */

			if (jstr(p1 ? p1 : fstart, day_end, "type", w.type,
				 sizeof(w.type)))
				;	/* 拿到 */

			if (jstr(p1 ? p1 : fstart, day_end, "high", tmp, sizeof(tmp)))
				w.high = parse_temp(tmp);
			if (jstr(p1 ? p1 : fstart, day_end, "low", tmp, sizeof(tmp)))
				w.low = parse_temp(tmp);

			/* 今天的 notice 更贴切，有就用它覆盖 ganmao */
			if (jstr(p1 ? p1 : fstart, day_end, "notice", tmp,
				 sizeof(tmp)))
				snprintf(w.notice, sizeof(w.notice), "%s", tmp);
		}

		/*
		 * 数据时间在顶层 "time":"2026-11-01 00:41:01"。
		 *
		 * ★ 原来只取时分（"00:41"），看起来够了，实际会误导：
		 *   今天的 00:41 和昨天的 00:41 显示完全一样，
		 *   用户没法判断"这数据是刚拿的还是昨天剩的"。
		 *
		 *   2026-11-01 就踩了这个：界面一直显示"29℃ 多云"，
		 *   用户以为是程序没刷新 —— 实际是数据源本身停更了，
		 *   接口返回的 date 还是昨天、updateTime 停在昨晚 20:08。
		 *   因为界面上没有任何时间信息，这个"数据陈旧"完全不可见，
		 *   只能靠人去猜"是不是坏了"。
		 *
		 *   现在保留日期：   "2026-11-01 00:41:01" → "11-01 00:41"
		 *   如果格式不是预期的（接口变动），就原样保留，不做裁剪。
		 */
		if (jstr(json, fstart, "time", tmp, sizeof(tmp))) {
			if (strlen(tmp) >= 16 && tmp[4] == '-' && tmp[10] == ' ') {
				snprintf(w.update_time, sizeof(w.update_time),
					 "%.5s %.5s", tmp + 5, tmp + 11);

				/* 同时算成 epoch，供 UI 判断"数据过旧"。
				 *
				 * ★ 这里用 mktime 而不是自己算时区偏移：
				 *   timesync_setup_tz() 已经 setenv("TZ","CST-8")+tzset()，
				 *   mktime 会按东八区解释这个 "2026-11-01 08:25:21"，
				 *   得到的 epoch 和 time(NULL) 直接可比，**不用手动 +8**。
				 *
				 *   反过来如果自己加 28800，一旦哪天 TZ 变了就会差 8 小时，
				 *   而那种错误在界面上看不出来（只会表现为"数据总是很新"
				 *   或"总是很旧"）。
				 */
				struct tm tm_ut = {0};

				if (sscanf(tmp, "%d-%d-%d %d:%d:%d",
					   &tm_ut.tm_year, &tm_ut.tm_mon, &tm_ut.tm_mday,
					   &tm_ut.tm_hour, &tm_ut.tm_min,
					   &tm_ut.tm_sec) == 6) {
					tm_ut.tm_year -= 1900;
					tm_ut.tm_mon -= 1;
					tm_ut.tm_isdst = -1;	/* 让 mktime 自己判断 */
					w.update_epoch = (long)mktime(&tm_ut);
				}
			} else {
				snprintf(w.update_time, sizeof(w.update_time),
					 "%s", tmp);
			}
		}
	}

	w.valid = w.city[0] != '\0' && w.temp != -99;

store:
	/* ★ 这里原来有 if (json) free(json) —— 现在由 jbuf 的析构负责。
	 *   注意 goto 跳到 store 时，jbuf 仍在作用域内，会被正确析构；
	 *   这正是 C++ 相比 goto+手工 free 更安全的地方：不需要"记得配对"。 */
	{
		std::lock_guard<std::mutex> lk(g_lock);

		g_w = w;
		g_busy = false;
		g_seq++;
	}

	if (w.valid)
		std::printf("[天气] %s %d℃ %s 湿度%d%% AQI%d(%s) %d~%d℃\n",
			    w.city, w.temp, w.type, w.humidity, w.aqi, w.quality,
			    w.low, w.high);
	else
		std::printf("[天气] 获取失败: %s\n", w.err);

	/* code 是值传递的 std::string，函数返回自动释放 —— 不需要 free(code) */
}

extern "C" void weather_init(void)
{
	std::memset(&g_w, 0, sizeof(g_w));
	g_w.aqi = -1;
	g_busy = false;
}

extern "C" void weather_fetch(const char *citycode)
{
	if (!citycode || !citycode[0])
		return;

	{
		std::lock_guard<std::mutex> lk(g_lock);

		if (g_busy)
			return;         /* 已经有一路在拉，直接忽略 */
		g_busy = true;
	}

	/*
	 * ★ 相比原来的 strdup + pthread_create + pthread_detach：
	 *
	 *   原写法要人肉保证三件事，漏一个就出事：
	 *     ① strdup 后必须 free（线程里做，容易忘）
	 *     ② pthread_create 失败必须 free(code)（这个原来做了）
	 *     ③ 参数内存必须比线程活得久 —— 所以不能传栈地址，才要 strdup
	 *
	 *   现在：lambda 按【值】捕获 std::string，拷贝进线程自己的存储，
	 *   生命周期由 std::thread 的栈帧保证。不需要 strdup、不需要 free。
	 *
	 *   std::thread 析构前必须 join 或 detach，否则会 terminate。
	 *   这里是"发出去就不管"的一次性任务，所以 detach。
	 */
	try {
		std::thread th([code = std::string(citycode)]() {
			fetch_thread(code);
		});

		th.detach();
	} catch (...) {
		/* 线程资源不足（板子只有 54MB，极端情况下会失败） */
		std::lock_guard<std::mutex> lk(g_lock);

		g_busy = false;
		std::snprintf(g_w.err, sizeof(g_w.err), "创建线程失败");
	}
}

extern "C" bool weather_busy(void)
{
	std::lock_guard<std::mutex> lk(g_lock);

	return g_busy;
}

extern "C" int weather_get(struct weather_info *out)
{
	std::lock_guard<std::mutex> lk(g_lock);

	std::memcpy(out, &g_w, sizeof(*out));
	return g_seq;
}
