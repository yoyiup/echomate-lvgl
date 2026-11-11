/**
 * cam_image.c —— NV12 转换 + BMP / AVI 输出
 *
 * 全文件零外部依赖，只用标准 C。
 */
#include "cam_image.h"

#include <cstdlib>
#include <cstring>

#include "resources.hpp"     /* res::File —— 自动 fclose */

/* ------------------------------------------------------------------ */
/* YUV → RGB 系数                                                     */
/* ------------------------------------------------------------------ */
/*
 * BT.601 limited range（视频惯例：Y 16~235，UV 128±112）。
 * 用整数定点：R = (298*(Y-16) + 409*(V-128)) >> 8
 *
 * ★ 为什么用 limited range 而不是 full range：
 *   摄像头/ISP 输出的 NV12 基本都是 limited range（这是视频标准）。
 *   如果按 full range 解，画面会整体偏亮、高光死白。
 *   判断依据：看 Y 的实际范围，如果最亮只到 235 左右就是 limited。
 */
#define CLIP255(x)	((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

static inline void yuv2rgb(int y, int u, int v, int *r, int *g, int *b)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;

	*r = CLIP255((298 * c + 409 * e + 128) >> 8);
	*g = CLIP255((298 * c - 100 * d - 208 * e + 128) >> 8);
	*b = CLIP255((298 * c + 516 * d + 128) >> 8);
}

/* ------------------------------------------------------------------ */
/* NV12 → RGB565                                                      */
/* ------------------------------------------------------------------ */
extern "C" void nv12_to_rgb565(const uint8_t *src_y, const uint8_t *src_uv,
		    int src_w, int src_h,
		    uint16_t *dst, int dw, int dh)
{
	int x, y;
	int sx, y0;

	/*
	 * ★ 保持宽高比 + 居中裁剪，不做拉伸。
	 *
	 *   这块屏的预览区是 320x160（2:1），而采集是 320x240（4:3）——
	 *   比例不一样。三种处理方式：
	 *     ① 直接压扁到 320x160  → 人会被压扁，明显变形
	 *     ② 拉伸铺满            → 同样变形
	 *     ③ 【居中裁剪】        → 取中间 320x160，上下各裁 40 行，
	 *                             1:1 像素映射，不变形也不用重采样 ✓
	 *
	 *   算法：先按宽度算出抽样步长 sx（320/320=1），
	 *   那么"用同样密度填充 dh 行"需要 dh*sx 行源数据；
	 *   从这个高度里居中取（y0 = 超出的部分对半分）。
	 *
	 *   320x240 → 320x160 时：sx=1，需要 160 行，y0=(240-160)/2=40。
	 *   640x480 → 320x160 时：sx=2，需要 320 行，y0=(480-320)/2=80。
	 *   两种情况都不缩放，纯像素搬运，CPU 最省。
	 */
	sx = src_w / dw;
	if (sx < 1)
		sx = 1;

	y0 = (src_h - dh * sx) / 2;
	if (y0 < 0)
		y0 = 0;

	for (y = 0; y < dh; y++) {
		int src_y_row = y0 + y * sx;
		const uint8_t *yp;
		const uint8_t *uvp;
		uint16_t *out;

		if (src_y_row >= src_h)
			break;		/* 源不够高就别读越界，剩下的留空 */

		yp = src_y + src_y_row * src_w;
		/* UV 平面是 Y 的 1/2 分辨率：行号要再除 2 */
		uvp = src_uv + (src_y_row / 2) * src_w;
		out = dst + y * dw;

		for (x = 0; x < dw; x++) {
			int src_x = x * sx;
			int Y, U, V, r, g, b;

			if (src_x >= src_w)
				break;
			Y = yp[src_x];
			/* UV 是交错存放：偶数下标是 U，奇数是 V */
			U = uvp[(src_x / 2) * 2];
			V = uvp[(src_x / 2) * 2 + 1];

			yuv2rgb(Y, U, V, &r, &g, &b);
			out[x] = (uint16_t)(((r & 0xF8) << 8) |
					    ((g & 0xFC) << 3) |
					    (b >> 3));
		}
	}
}

/* ------------------------------------------------------------------ */
/* NV12 → BGR24                                                       */
/* ------------------------------------------------------------------ */
extern "C" void nv12_to_bgr24(const uint8_t *src_y, const uint8_t *src_uv,
		   int src_w, int src_h,
		   uint8_t *dst, int dw, int dh)
{
	int x, y;
	int sx = src_w / dw;
	int sy = src_h / dh;

	if (sx < 1)
		sx = 1;
	if (sy < 1)
		sy = 1;

	for (y = 0; y < dh; y++) {
		int src_y_row = y * sy;
		const uint8_t *yp = src_y + src_y_row * src_w;
		const uint8_t *uvp = src_uv + (src_y_row / 2) * src_w;
		uint8_t *out = dst + y * dw * 3;

		for (x = 0; x < dw; x++) {
			int src_x = x * sx;
			int Y = yp[src_x];
			int U = uvp[(src_x / 2) * 2];
			int V = uvp[(src_x / 2) * 2 + 1];
			int r, g, b;

			yuv2rgb(Y, U, V, &r, &g, &b);
			/* BGR 顺序：BMP 和 AVI 的 BI_RGB 都是这个顺序 */
			out[x * 3 + 0] = (uint8_t)b;
			out[x * 3 + 1] = (uint8_t)g;
			out[x * 3 + 2] = (uint8_t)r;
		}
	}
}

extern "C" void nv12_to_rgb24(const uint8_t *src_y, const uint8_t *src_uv,
		   int src_w, int src_h,
		   uint8_t *dst, int dw, int dh)
{
	int x, y;
	int sx = src_w / dw;
	int sy = src_h / dh;

	if (sx < 1)
		sx = 1;
	if (sy < 1)
		sy = 1;

	for (y = 0; y < dh; y++) {
		int src_y_row = y * sy;
		const uint8_t *yp = src_y + src_y_row * src_w;
		const uint8_t *uvp = src_uv + (src_y_row / 2) * src_w;
		uint8_t *out = dst + y * dw * 3;

		for (x = 0; x < dw; x++) {
			int src_x = x * sx;
			int Y = yp[src_x];
			int U = uvp[(src_x / 2) * 2];
			int V = uvp[(src_x / 2) * 2 + 1];
			int r, g, b;

			yuv2rgb(Y, U, V, &r, &g, &b);
			/* RGB 顺序：YOLO 训练时用的就是 RGB，别换成 BGR */
			out[x * 3 + 0] = (uint8_t)r;
			out[x * 3 + 1] = (uint8_t)g;
			out[x * 3 + 2] = (uint8_t)b;
		}
	}
}

/* ------------------------------------------------------------------ */
/* BMP（24 位，未压缩）                                                */
/* ------------------------------------------------------------------ */
static void put_u16(uint8_t *p, uint16_t v){
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

extern "C" int bmp_write(const char *path, const uint8_t *bgr, int w, int h)
{
	/* 每行必须 4 字节对齐 —— BMP 规范要求，不对齐部分播放器会读歪 */
	int row_raw = w * 3;
	int row_pad = (4 - (row_raw % 4)) % 4;
	int row_sz = row_raw + row_pad;
	uint32_t img_sz = (uint32_t)row_sz * h;
	uint32_t file_sz = 54 + img_sz;
	uint8_t hdr[54];
	static const uint8_t pad[4] = { 0, 0, 0, 0 };
	int y;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'B';
	hdr[1] = 'M';
	put_u32(hdr + 2, file_sz);
	put_u32(hdr + 10, 54);			/* 像素数据偏移 */
	put_u32(hdr + 14, 40);			/* BITMAPINFOHEADER 大小 */
	put_u32(hdr + 18, (uint32_t)w);
	put_u32(hdr + 22, (uint32_t)h);		/* 正数 = 自下而上 */
	put_u16(hdr + 26, 1);			/* planes */
	put_u16(hdr + 28, 24);			/* 每像素位数 */
	put_u32(hdr + 34, img_sz);

	/*
	 * ★ 原来是 fopen 之后有 3 条 return 路径（写头失败、循环里 break、
	 *   正常结束），fclose 要写两处。改成 res::File 后一处都不用写 ——
	 *   连"写头失败要先 fclose 再 return"这种细节也消失了。
	 */
	res::File fp(std::fopen(path, "wb"));

	if (!fp)
		return -1;

	if (std::fwrite(hdr, 1, 54, fp.get()) != 54)
		return -1;

	/*
	 * ★ BMP 的行序是【自下而上】。
	 *   所以要从最后一行开始写。写成自上而下不会报错，
	 *   只会得到一张上下颠倒的图 —— 这个坑很常见，
	 *   看到"图是倒的"就是这里错了，不是摄像头装反了。
	 */
	for (y = h - 1; y >= 0; y--) {
		const uint8_t *row = bgr + (size_t)y * row_raw;

		if (std::fwrite(row, 1, row_raw, fp.get()) != (size_t)row_raw)
			break;
		if (row_pad &&
		    std::fwrite(pad, 1, row_pad, fp.get()) != (size_t)row_pad)
			break;
	}

	/* fp 析构时自动 fclose；y<0 表示整个循环跑完了（没 break） */
	return (y < 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* AVI（未压缩 BGR）                                                   */
/* ------------------------------------------------------------------ */
/*
 * AVI 结构（只需要最小子集，播放器就认）：
 *
 *   RIFF____AVI
 *     LIST____hdrl
 *       avih         主头（宽高、帧率、总帧数）
 *       LIST____strl
 *         strh       流头（视频流、编解码器 = DIB）
 *         strf       BITMAPINFOHEADER
 *     LIST____movi
 *       00db + 帧   每帧一个 chunk
 *     idx1           索引（每帧 16 字节）
 *
 * 所有多字节都是小端。
 */

/*
 * ★ C++ 的列表初始化比 C 严格：`uint8_t b[4] = { v & 0xff, ... }` 里
 *   v 是 uint32_t，右值到 uint8_t 属于【窄化转换】，C++ 直接报
 *   -Wnarrowing（C 允许隐式截断）。
 *
 *   这里显式 static_cast<uint8_t> 表明"我知道会截断，就是要低 8 位"。
 *   别图省事改成 = 赋值（那样会退回隐式转换，警告消失但意图不明确）。
 */
static void w32(FILE *fp, uint32_t v)
{
	uint8_t b[4] = {
		static_cast<uint8_t>(v & 0xff),
		static_cast<uint8_t>((v >> 8) & 0xff),
		static_cast<uint8_t>((v >> 16) & 0xff),
		static_cast<uint8_t>((v >> 24) & 0xff),
	};

	std::fwrite(b, 1, 4, fp);
}

static void w16(FILE *fp, uint16_t v)
{
	uint8_t b[2] = {
		static_cast<uint8_t>(v & 0xff),
		static_cast<uint8_t>((v >> 8) & 0xff),
	};

	std::fwrite(b, 1, 2, fp);
}

static void wcc(FILE *fp, const char *cc)
{
	fwrite(cc, 1, 4, fp);
}

extern "C" int avi_open(avi_writer *w, const char *path, int width, int height, int fps)
{
	uint32_t usec_per_frame;

	memset(w, 0, sizeof(*w));
	w->width = width;
	w->height = height;
	w->fps = fps > 0 ? fps : 8;
	w->frame_bytes = (uint32_t)width * height * 3;
	usec_per_frame = 1000000u / (uint32_t)w->fps;

	w->fp = fopen(path, "wb+");	/* 必须能 seek，末尾要回填 */
	if (!w->fp)
		return -1;

	/* ---- RIFF 头（大小稍后回填）---- */
	wcc(w->fp, "RIFF");
	w32(w->fp, 0);			/* 回填点 @4 */
	wcc(w->fp, "AVI ");

	/* ---- LIST hdrl（大小稍后回填）---- */
	wcc(w->fp, "LIST");
	w32(w->fp, 0);			/* 回填点 @12 */
	wcc(w->fp, "hdrl");

	/* ---- avih：主头 56 字节 ---- */
	wcc(w->fp, "avih");
	w32(w->fp, 56);
	w32(w->fp, usec_per_frame);	/* dwMicroSecPerFrame */
	w32(w->fp, w->frame_bytes * w->fps); /* dwMaxBytesPerSec（估算） */
	w32(w->fp, 0);			/* dwPaddingGranularity */
	w32(w->fp, 0x10);		/* dwFlags: AVIF_HASINDEX */
	w32(w->fp, 0);			/* dwTotalFrames  ← 回填点 @32 */
	w32(w->fp, 0);			/* dwInitialFrames */
	w32(w->fp, 1);			/* dwStreams */
	w32(w->fp, w->frame_bytes);	/* dwSuggestedBufferSize */
	w32(w->fp, (uint32_t)width);
	w32(w->fp, (uint32_t)height);
	w32(w->fp, 0); w32(w->fp, 0); w32(w->fp, 0); w32(w->fp, 0); /* reserved[4] */

	/* ---- LIST strl ---- */
	wcc(w->fp, "LIST");
	w32(w->fp, 4 + 8 + 56 + 8 + 40);
	wcc(w->fp, "strl");

	/* ---- strh：流头 56 字节 ---- */
	wcc(w->fp, "strh");
	w32(w->fp, 56);
	wcc(w->fp, "vids");		/* fccType */
	wcc(w->fp, "DIB ");		/* fccHandler：未压缩位图 */
	w32(w->fp, 0);			/* dwFlags */
	w16(w->fp, 0);			/* wPriority */
	w16(w->fp, 0);			/* wLanguage */
	w32(w->fp, 0);			/* dwInitialFrames */
	w32(w->fp, 1);			/* dwScale  —— 与 dwRate 一起表示帧率 */
	w32(w->fp, (uint32_t)w->fps);	/* dwRate */
	w32(w->fp, 0);			/* dwStart */
	w32(w->fp, 0);			/* dwLength ← 回填点（总帧数） */
	w32(w->fp, w->frame_bytes);	/* dwSuggestedBufferSize */
	w32(w->fp, 0xFFFFFFFF);		/* dwQuality */
	w32(w->fp, 0);			/* dwSampleSize */
	w16(w->fp, 0); w16(w->fp, 0);	/* rcFrame: left/top */
	w16(w->fp, (uint16_t)width);
	w16(w->fp, (uint16_t)height);

	/* ---- strf：BITMAPINFOHEADER 40 字节 ---- */
	wcc(w->fp, "strf");
	w32(w->fp, 40);
	w32(w->fp, 40);
	w32(w->fp, (uint32_t)width);
	w32(w->fp, (uint32_t)height);	/* 正数：自下而上（BMP 惯例） */
	w16(w->fp, 1);			/* biPlanes */
	w16(w->fp, 24);			/* biBitCount */
	w32(w->fp, 0);			/* BI_RGB = 0，未压缩 */
	w32(w->fp, w->frame_bytes);
	w32(w->fp, 0); w32(w->fp, 0); w32(w->fp, 0); w32(w->fp, 0);

	/* ---- LIST movi（大小稍后回填）---- */
	wcc(w->fp, "LIST");
	w32(w->fp, 0);			/* 回填点 */
	wcc(w->fp, "movi");
	w->movi_start = (uint32_t)ftell(w->fp) - 4;	/* 指向 'movi' */

	return 0;
}

extern "C" int avi_add_frame(avi_writer *w, const uint8_t *bgr)
{
	int y;
	int row = w->width * 3;

	if (!w->fp)
		return -1;

	wcc(w->fp, "00db");
	w32(w->fp, w->frame_bytes);

	/* 与 BMP 一致：AVI 的 DIB 也是自下而上 */
	for (y = w->height - 1; y >= 0; y--)
		fwrite(bgr + (size_t)y * row, 1, row, w->fp);

	w->frames++;
	return 0;
}

extern "C" int avi_close(avi_writer *w)
{
	long movi_end;
	uint32_t movi_sz, riff_sz, i;

	if (!w->fp)
		return -1;

	/*
	 * ★ movi 的结束位置必须在写 idx1【之前】记下来。
	 *   movi LIST 只包住数据帧，idx1 是 movi 的兄弟不是子节点。
	 *   先写 idx1 再算 movi 大小，会把索引也算进去 —— 播放器读到
	 *   多出来的字节，表现是"能打开但播到一半就停"或直接报错。
	 */
	movi_end = ftell(w->fp);

	/* ---- idx1 索引：每帧一条 16 字节 ---- */
	wcc(w->fp, "idx1");
	w32(w->fp, w->frames * 16);
	for (i = 0; i < w->frames; i++) {
		wcc(w->fp, "00db");
		w32(w->fp, 0x10);	/* AVIIF_KEYFRAME */
		/*
		 * 偏移按规范是"相对 'movi' fourcc 的位置"，
		 * 第一帧的数据在一个 8 字节 chunk 头之后，所以是 4 + 8 = 12 起步。
		 * 有些播放器容忍写成 0 起步，但严格按规范才能通吃。
		 */
		w32(w->fp, 4 + i * (8 + w->frame_bytes) + 8);
		w32(w->fp, w->frame_bytes);
	}

	/* ---- 回填 movi LIST 的 size ---- */
	/* size 字段位于 'movi' 的 fourcc 之前 4 字节（即 LIST 头里的那个位置） */
	movi_sz = (uint32_t)(movi_end - (long)w->movi_start) + 4;
	fseek(w->fp, (long)w->movi_start - 4, SEEK_SET);
	w32(w->fp, movi_sz);

	/* ---- 回填 hdrl LIST 的 size ----
	 * 'hdrl' 的 LIST 从偏移 12 开始，它的 size 字段在偏移 16。
	 * size = 从 'hdrl' fourcc 起，到 'movi' LIST 出现之前的总长度。
	 */
	fseek(w->fp, 16, SEEK_SET);
	w32(w->fp, (uint32_t)(w->movi_start - 8 - 16));

	/* ---- 回填总帧数 ----
	 * avih.dwTotalFrames  在偏移 48
	 * strh.dwLength       在偏移 140
	 *   avih 数据 32..88，然后 LIST strl(12) + strh头(8) → 108 起是 strh 数据
	 *   dwFlags(4) wPriority(2) wLanguage(2) dwInitialFrames(4) dwScale(4) dwRate(4)
	 *   → dwRate 在 132，dwStart 在 136，dwLength 在 140
	 */
	fseek(w->fp, 48, SEEK_SET);
	w32(w->fp, w->frames);
	fseek(w->fp, 140, SEEK_SET);
	w32(w->fp, w->frames);

	/* ---- 回填 RIFF 总大小 = 文件长度 - 8 ---- */
	fseek(w->fp, 0, SEEK_END);
	riff_sz = (uint32_t)ftell(w->fp) - 8;
	fseek(w->fp, 4, SEEK_SET);
	w32(w->fp, riff_sz);

	fseek(w->fp, 0, SEEK_END);
	fclose(w->fp);
	w->fp = NULL;
	return 0;
}
