/**
 * yolo_detect.cpp —— YOLOv5 RKNN 推理（RV1106 NPU，零拷贝）
 *
 * 【和原厂 demo 的关系】
 *   模型解码那套公式（anchor / 2.0-0.5 / 平方 / stride）来自
 *   Demo/yolov5_demo/cpp 的 process_i8_rv1106()。那套公式是跟模型一起
 *   量化的，不能自己瞎改；但 demo 用 opencv-mobile 抓图，我们不需要，
 *   所以只把「解码 + NMS」这部分搬过来，前后自己接。
 *
 * 【RV1106 只能用零拷贝接口】
 *   输入输出都用 rknn_create_mem 分配、rknn_set_io_mem 绑定，
 *   然后直接往 mem->virt_addr 里写像素。不能用 rknn_inputs_set。
 *   输入张量类型必须是 UINT8（归一化/量化交给 NPU 内部做）。
 *
 * 【输出格式（实测）】
 *   [1,80,80,255] [1,40,40,255] [1,20,20,255]，int8，zp=-128，scale=1/255
 *   255 = 3 个 anchor × 85（4 box + 1 obj + 80 cls），NHWC 连续排列。
 */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include <unistd.h>

#include "rknn_api.h"

#include "yolo_detect.h"
#include "../cam/cam.h"

namespace {

/* ---- 模型固定参数（和 yolov5.rknn 一起定死，换模型要同步改）---- */
constexpr int   kNetW		= 640;
constexpr int   kNetH		= 640;
constexpr int   kPropBox	= 85;			/* 4 + 1 + 80 */
constexpr int   kAnchorPerBranch = 3;
constexpr int   kAlignC		= kPropBox * kAnchorPerBranch;	/* 255 */
constexpr float kConfThresh	= 0.25f;
constexpr float kNmsThresh	= 0.45f;
constexpr int   kPadValue	= 114;			/* letterbox 填充灰，和训练一致 */

/* yolov5s 的三组 anchor（每个 stride 三个） */
const int kAnchor[3][6] = {
	{ 10, 13, 16, 30, 33, 23 },	/* stride 8  */
	{ 30, 61, 62, 45, 59, 119 },	/* stride 16 */
	{ 116, 90, 156, 198, 373, 326 },	/* stride 32 */
};

std::atomic<bool>	g_run{ false };
std::thread		g_th;
std::atomic<uint32_t>	g_seq{ 0 };

std::mutex		g_res_lock;
yolo_result		g_res{};
yolo_result		g_res_pub{};		/* UI 读到的那份 */

std::mutex		g_txt_lock;
char			g_status[96] = "未启动";
char			g_err[160];
std::vector<std::string> g_labels;

rknn_context		g_ctx = 0;
rknn_tensor_mem	       *g_in_mem = nullptr;
rknn_tensor_mem	       *g_out_mem[3] = { nullptr, nullptr, nullptr };
rknn_tensor_attr	g_out_attr[3]{};
int			g_out_n = 0;
float			g_zp = -128.f;
float			g_scale = 1.f / 255.f;

std::vector<uint8_t>	g_src;			/* 采集图 RGB888 快照 */
uint8_t		       *g_in_ptr = nullptr;	/* NPU 输入缓冲（零拷贝）*/

void set_status(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	std::lock_guard<std::mutex> lk(g_txt_lock);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
}

void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	std::lock_guard<std::mutex> lk(g_txt_lock);
	vsnprintf(g_err, sizeof(g_err), fmt, ap);
	va_end(ap);
	std::printf("[YOLO] %s\n", g_err);
}

double now_ms()
{
	using namespace std::chrono;

	return duration_cast<duration<double, std::milli>>(
		       steady_clock::now().time_since_epoch()).count();
}

/* ---------------------------------------------------------------- */
/* 模型                                                            */
/* ---------------------------------------------------------------- */

bool load_labels(const char *path)
{
	FILE *f;
	char line[64];

	if (!path)
		return false;

	f = std::fopen(path, "r");
	if (!f)
		return false;

	while (std::fgets(line, sizeof(line), f)) {
		size_t n = std::strlen(line);

		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (n)
			g_labels.emplace_back(line);
	}
	std::fclose(f);
	return !g_labels.empty();
}

bool model_init(const char *model_path)
{
	rknn_input_output_num io_num;
	rknn_tensor_attr in_attr;
	int ret;

	ret = rknn_init(&g_ctx, (char *)model_path, 0, 0, nullptr);
	if (ret < 0) {
		set_error("rknn_init 失败 (%d)：模型路径或 librknnmrt.so / rknpu 驱动",
			  ret);
		return false;
	}

	ret = rknn_query(g_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
	if (ret != RKNN_SUCC || io_num.n_output < 3) {
		set_error("rknn_query 输入输出数量失败 (%d)", ret);
		return false;
	}

	/* ---- 输入：640x640x3 UINT8 NHWC ---- */
	std::memset(&in_attr, 0, sizeof(in_attr));
	in_attr.index = 0;
	rknn_query(g_ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &in_attr,
		   sizeof(rknn_tensor_attr));
	in_attr.type = RKNN_TENSOR_UINT8;
	in_attr.fmt  = RKNN_TENSOR_NHWC;

	if (in_attr.dims[1] != kNetH || in_attr.dims[2] != kNetW) {
		set_error("模型输入不是 640x640（实际 %dx%d），参数要跟着改",
			  in_attr.dims[2], in_attr.dims[1]);
		return false;
	}

	g_in_mem = rknn_create_mem(g_ctx, in_attr.size_with_stride);
	if (!g_in_mem) {
		set_error("输入内存分配失败（CMA 不够？）");
		return false;
	}
	if (rknn_set_io_mem(g_ctx, g_in_mem, &in_attr) < 0) {
		set_error("rknn_set_io_mem(输入) 失败");
		return false;
	}
	g_in_ptr = static_cast<uint8_t *>(g_in_mem->virt_addr);

	/* ---- 输出：三个检测头 ---- */
	g_out_n = io_num.n_output > 3 ? 3 : static_cast<int>(io_num.n_output);
	for (int i = 0; i < g_out_n; i++) {
		std::memset(&g_out_attr[i], 0, sizeof(rknn_tensor_attr));
		g_out_attr[i].index = i;
		ret = rknn_query(g_ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR,
				 &g_out_attr[i], sizeof(rknn_tensor_attr));
		if (ret != RKNN_SUCC) {
			set_error("rknn_query 输出[%d] 失败 (%d)", i, ret);
			return false;
		}
		g_out_mem[i] = rknn_create_mem(g_ctx,
					       g_out_attr[i].size_with_stride);
		if (!g_out_mem[i]) {
			set_error("输出[%d] 内存分配失败", i);
			return false;
		}
		if (rknn_set_io_mem(g_ctx, g_out_mem[i], &g_out_attr[i]) < 0) {
			set_error("rknn_set_io_mem(输出 %d) 失败", i);
			return false;
		}
	}

	/* 量化参数取第一个输出头的（三个头一致，实测都是 -128 / 1/255）*/
	g_zp    = static_cast<float>(g_out_attr[0].zp);
	g_scale = g_out_attr[0].scale;

	return true;
}

void model_deinit()
{
	for (int i = 0; i < g_out_n; i++) {
		if (g_out_mem[i]) {
			rknn_destroy_mem(g_ctx, g_out_mem[i]);
			g_out_mem[i] = nullptr;
		}
	}
	if (g_in_mem) {
		rknn_destroy_mem(g_ctx, g_in_mem);
		g_in_mem = nullptr;
	}
	if (g_ctx) {
		rknn_destroy(g_ctx);
		g_ctx = 0;
	}
	g_in_ptr = nullptr;
	g_out_n = 0;
}

/* ---------------------------------------------------------------- */
/* 预处理：letterbox 到 640x640                                     */
/* ---------------------------------------------------------------- */

/*
 * 把 320x240 的 RGB888 等比缩放后放进 640x640 的正方形，四周填灰。
 *
 * 【为什么不能直接拉伸到 640x640】
 *   320x240 是 4:3，硬拉到 1:1 会让画面横竖比例失真，
 *   人变胖、圆变椭圆 —— 模型是按正常比例训练的，这样检测框会明显变差。
 *
 * 【为什么四周填 114 而不是黑色】
 *   YOLO 训练时的 letterbox 就是填 (114,114,114)，推理必须一致。
 *   填黑会在图像边界造出一圈"纯黑边"，模型容易把边上的物体判错。
 *
 * 【缩放用最近邻】
 *   320→640 是 2 倍放大，本身信息就那么多，双线性也只是把块变糊，
 *   检测精度提升有限，反而多花几毫秒 CPU —— 单核 A7 上不值得。
 */
void letterbox(const uint8_t *src, int sw, int sh,
	       int *out_pad_x, int *out_pad_y, float *out_ratio)
{
	float ratio = static_cast<float>(kNetW) / sw;

	if (static_cast<float>(kNetH) / sh < ratio)
		ratio = static_cast<float>(kNetH) / sh;

	const int nw = static_cast<int>(sw * ratio);
	const int nh = static_cast<int>(sh * ratio);
	const int pad_x = (kNetW - nw) / 2;
	const int pad_y = (kNetH - nh) / 2;

	*out_pad_x = pad_x;
	*out_pad_y = pad_y;
	*out_ratio = ratio;

	std::memset(g_in_ptr, kPadValue, static_cast<size_t>(kNetW) * kNetH * 3);
	if (sw == 320 && sh == 240 && nw == 640 && nh == 480) {
		/* 常用尺寸正好放大两倍：每行复制一次，下一行直接 memcpy。 */
		for (int y = 0; y < sh; y++) {
			const uint8_t *sp = src + static_cast<size_t>(y) * sw * 3;
			uint8_t *dp = g_in_ptr +
				(static_cast<size_t>(y * 2 + pad_y) * kNetW + pad_x) * 3;
			for (int x = 0; x < sw; x++) {
				const uint8_t *p = sp + x * 3;
				uint8_t *q = dp + x * 6;
				q[0] = q[3] = p[0];
				q[1] = q[4] = p[1];
				q[2] = q[5] = p[2];
			}
			std::memcpy(dp + kNetW * 3, dp, kNetW * 3);
		}
		return;
	}

	for (int y = 0; y < nh; y++) {
		const int sy = static_cast<int>(y / ratio);

		if (sy >= sh)
			break;

		const uint8_t *sp = src + static_cast<size_t>(sy) * sw * 3;
		uint8_t *dp = g_in_ptr + (static_cast<size_t>(y + pad_y) * kNetW +
					  pad_x) * 3;

		for (int x = 0; x < nw; x++) {
			const int sx = static_cast<int>(x / ratio);
			const uint8_t *s = sp + static_cast<size_t>(sx) * 3;

			dp[x * 3 + 0] = s[0];
			dp[x * 3 + 1] = s[1];
			dp[x * 3 + 2] = s[2];
		}
	}
}

/* ---------------------------------------------------------------- */
/* 后处理：解码 + NMS                                              */
/* ---------------------------------------------------------------- */

inline float deq(int8_t q)
{
	return (static_cast<float>(q) - g_zp) * g_scale;
}

/*
 * 解一个检测头。
 * 布局是 NHWC 连续的：每个格子连着 255 个 int8（3 个 anchor × 85），
 * 所以偏移是 (h*gw + w) * 255 + a * 85 —— 不是"按通道切三段"。
 */
int decode_head(const int8_t *p, const int *anchor, int gh, int gw, int stride,
		std::vector<yolo_box> &out)
{
	/*
	 * ★ 量化阈值要用「浮点 → affine 整数」的正向公式：
	 *       q = f32 / scale + zp
	 *   不是 (f32 - zp) / scale —— 后者是我第一版写错的，
	 *   算出来 32704 强转 int8 直接溢出，结果是「一个框都检不出来」。
	 *   参考 demo 的 qnt_f32_to_affine()。
	 */
	int th_i = static_cast<int>(kConfThresh / g_scale + g_zp);

	if (th_i < -128)
		th_i = -128;
	if (th_i > 127)
		th_i = 127;

	const int8_t th = static_cast<int8_t>(th_i);
	int found = 0;

	for (int h = 0; h < gh; h++) {
		for (int w = 0; w < gw; w++) {
			for (int a = 0; a < kAnchorPerBranch; a++) {
				const int off = (h * gw + w) * kAlignC + a * kPropBox;
				const int8_t *q = p + off;

				if (q[4] < th)
					continue;

				int best = 0;
				int8_t best_p = q[5];

				for (int k = 1; k < YOLO_CLASS_NUM; k++) {
					if (q[5 + k] > best_p) {
						best_p = q[5 + k];
						best = k;
					}
				}

				const float box_c = deq(q[4]);
				const float cls_p = deq(best_p);
				const float score = box_c * cls_p;

				if (score < kConfThresh)
					continue;

				float bx = deq(q[0]) * 2.0f - 0.5f;
				float by = deq(q[1]) * 2.0f - 0.5f;
				float bw = deq(q[2]) * 2.0f;
				float bh = deq(q[3]) * 2.0f;

				bx = (bx + w) * stride;
				by = (by + h) * stride;
				bw = bw * bw * anchor[a * 2];
				bh = bh * bh * anchor[a * 2 + 1];
				bx -= bw / 2.0f;
				by -= bh / 2.0f;

				if (out.size() >= 256)	/* 兜底，别让极端画面撑爆内存 */
					continue;

				yolo_box b{};

				b.x = static_cast<int>(bx);
				b.y = static_cast<int>(by);
				b.w = static_cast<int>(bw);
				b.h = static_cast<int>(bh);
				b.cls = best;
				b.prop = score;
				out.push_back(b);
				found++;
			}
		}
	}
	return found;
}

float iou(const yolo_box &a, const yolo_box &b)
{
	const int x1 = std::max(a.x, b.x);
	const int y1 = std::max(a.y, b.y);
	const int x2 = std::min(a.x + a.w, b.x + b.w);
	const int y2 = std::min(a.y + a.h, b.y + b.h);
	const int iw = x2 - x1;
	const int ih = y2 - y1;

	if (iw <= 0 || ih <= 0)
		return 0.f;

	const float inter = static_cast<float>(iw) * ih;

	return inter / (static_cast<float>(a.w) * a.h +
			static_cast<float>(b.w) * b.h - inter);
}

/* 按分数从高到低，逐个保留、压掉和它重叠太多的（同类才压） */
void nms(std::vector<yolo_box> &boxes)
{
	for (size_t i = 0; i < boxes.size(); i++) {
		for (size_t j = i + 1; j < boxes.size();) {
			if (boxes[i].cls == boxes[j].cls &&
			    iou(boxes[i], boxes[j]) > kNmsThresh)
				boxes.erase(boxes.begin() + j);
			else
				j++;
		}
	}
}

/* 模型坐标 → 采集图坐标（逆着 letterbox 走一遍） */
void map_back(std::vector<yolo_box> &boxes, int pad_x, int pad_y, float ratio,
	      int sw, int sh)
{
	for (auto &b : boxes) {
		b.x = static_cast<int>((b.x - pad_x) / ratio);
		b.y = static_cast<int>((b.y - pad_y) / ratio);
		b.w = static_cast<int>(b.w / ratio);
		b.h = static_cast<int>(b.h / ratio);

		if (b.x < 0)
			b.x = 0;
		if (b.y < 0)
			b.y = 0;
		if (b.x + b.w > sw)
			b.w = sw - b.x;
		if (b.y + b.h > sh)
			b.h = sh - b.y;
	}
}

/* ---------------------------------------------------------------- */
/* 推理线程                                                        */
/* ---------------------------------------------------------------- */

void infer_loop()
{
	int sw = 0, sh = 0;
	uint32_t last_seq = 0;
	int frames = 0;
	double fps_t0 = now_ms();

	cam_capture_size(&sw, &sh);
	if (sw <= 0 || sh <= 0) {
		set_error("拿不到采集尺寸");
		g_run.store(false);
		return;
	}

	g_src.assign(static_cast<size_t>(sw) * sh * 3, 0);

	while (g_run.load(std::memory_order_acquire)) {
		const uint32_t s = cam_frame_rgb(g_src.data(),
						 static_cast<int>(g_src.size()));

		if (s == 0 || s == last_seq) {
			usleep(10 * 1000);
			continue;
		}
		last_seq = s;

		const double t0 = now_ms();

		int pad_x = 0, pad_y = 0;
		float ratio = 1.f;

		letterbox(g_src.data(), sw, sh, &pad_x, &pad_y, &ratio);
		const double prep_ms = now_ms() - t0;

		if (rknn_run(g_ctx, nullptr) < 0) {
			set_error("rknn_run 失败");
			break;
		}
		const double run_ms = now_ms() - t0 - prep_ms;

		std::vector<yolo_box> boxes;

		boxes.reserve(64);
		for (int i = 0; i < g_out_n; i++) {
			const int gh = g_out_attr[i].dims[2];
			const int gw = g_out_attr[i].dims[1];
			const int stride = kNetH / gh;

			decode_head(static_cast<int8_t *>(g_out_mem[i]->virt_addr),
				    kAnchor[i], gh, gw, stride, boxes);
		}

		/* 分数高的排前面，NMS 和"只画前 N 个"都依赖这个顺序 */
		std::sort(boxes.begin(), boxes.end(),
			  [](const yolo_box &a, const yolo_box &b) {
				  return a.prop > b.prop;
			  });
		nms(boxes);
		map_back(boxes, pad_x, pad_y, ratio, sw, sh);

		const int ms = static_cast<int>(now_ms() - t0);

		{
			std::lock_guard<std::mutex> lk(g_res_lock);
			const int n = std::min<int>(
				static_cast<int>(boxes.size()), YOLO_MAX_BOX);

			g_res.count = n;
			for (int i = 0; i < n; i++)
				g_res.box[i] = boxes[i];
			g_res.infer_ms = ms;
			g_res.src_w = sw;
			g_res.src_h = sh;
			g_res.seq = g_seq.load() + 1;
		}
		g_seq.fetch_add(1, std::memory_order_release);

		frames++;
		if (now_ms() - fps_t0 >= 2000) {
			const double fps = frames * 1000.0 / (now_ms() - fps_t0);
			int cnt;

			{
				std::lock_guard<std::mutex> lk(g_res_lock);
				cnt = g_res.count;
			}
			set_status("%.1f fps · %d 个目标 · 推理 %d ms", fps, cnt, ms);
			if (std::getenv("YOLO_STATS_LOG"))
				std::printf("[yolo-timing] prep=%.1f rknn=%.1f post=%.1f ms\n",
					    prep_ms, run_ms, ms - prep_ms - run_ms);
			frames = 0;
			fps_t0 = now_ms();
		}
	}
}

} /* namespace */

/* ---------------------------------------------------------------- */
/* C 接口                                                          */
/* ---------------------------------------------------------------- */

int yolo_start(const char *model_path, const char *labels_path)
{
	if (g_run.load())
		return 0;

	if (!cam_running()) {
		set_error("相机没启动，先 cam_start()");
		return -EINVAL;
	}

	g_labels.clear();
	load_labels(labels_path);

	if (!model_init(model_path))
		return -EIO;

	g_run.store(true, std::memory_order_release);
	set_status("启动中…");

	try {
		g_th = std::thread(infer_loop);
	} catch (...) {
		g_run.store(false);
		set_error("创建推理线程失败（内存不足）");
		model_deinit();
		return -ENOMEM;
	}

	std::printf("[YOLO] 已启动，模型 %s，%d 个类别名\n", model_path,
		    static_cast<int>(g_labels.size()));
	return 0;
}

void yolo_stop(void)
{
	const bool was_running = g_run.exchange(false);
	if (!was_running && !g_th.joinable())
		return;

	if (g_th.joinable())
		g_th.join();

	model_deinit();
	g_labels.clear();

	std::lock_guard<std::mutex> lk(g_res_lock);

	g_res = {};
	g_seq.store(0);
	set_status("未启动");
}

int yolo_running(void)
{
	return g_run.load() ? 1 : 0;
}

uint32_t yolo_get(yolo_result *out)
{
	uint32_t s;

	if (!out)
		return 0;

	s = g_seq.load(std::memory_order_acquire);
	if (s == 0)
		return 0;

	std::lock_guard<std::mutex> lk(g_res_lock);

	*out = g_res;
	out->seq = s;
	return s;
}

const char *yolo_class_name(int id)
{
	static thread_local char buf[24];

	if (id >= 0 && id < static_cast<int>(g_labels.size()))
		return g_labels[id].c_str();

	std::snprintf(buf, sizeof(buf), "cls%d", id);
	return buf;
}

const char *yolo_status(void)
{
	return g_status;
}

const char *yolo_last_error(void)
{
	return g_err;
}
