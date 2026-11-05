/**
 * cam_image.h —— 图像格式转换 + 文件输出（纯 C，零外部依赖）
 *
 * 【为什么全部手写而不找库】
 * 板子的 sysroot 里没有 libjpeg、没有 librga。而我们要的只是：
 *   · NV12 → RGB565（给 LVGL canvas，这块屏就是 RGB565）
 *   · NV12 → BGR24（给 BMP / AVI）
 *   · 存一张 BMP、存一段 AVI
 * 这三件事加起来不到 250 行，比拉一个库进来交叉编译省事得多，
 * 而且零依赖 = 不会在板上因为缺 .so 跑不起来。
 *
 * 【格式选择】
 *   拍照 → BMP：24 位未压缩，"任何"看图软件/手机都能开，零编码成本。
 *   录像 → AVI + 未压缩 BGR（BI_RGB）：这是 AVI 规范里的原生格式，
 *          VLC / ffplay / Windows 播放器都认。
 *          没选 MJPEG 是因为板上没有 JPEG 编码器；没选 H.264 是因为
 *          要用 mpp_vcodec 硬件编码，得再拉一整套 rockit 框架，不值得。
 *          代价是文件大，所以录像有时长上限（见 cam.c）。
 */

#ifndef __CAM_IMAGE_H__
#define __CAM_IMAGE_H__

#include <stdint.h>
#include <stdio.h>

/*
 * ★ C / C++ 混编必须加这段。
 *
 *   cam_image.c 是 C 编的，符号名就是 nv12_to_rgb565；
 *   而 C++ 编译器会做名字修饰（_Z16nv12_to_rgb565PKhS0_iiPthh 这种），
 *   不声明 extern "C" 的话，从 .cpp 里调就会链接失败：
 *       undefined reference to `nv12_to_rgb565(...)'
 *
 *   注意报错里那个符号【带着完整参数列表】，这就是没做 C 链接的典型特征。
 *   看到这种错误先想"是不是漏了 extern C"，别去查函数是不是没实现。
 */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * NV12(YUV420SP) → RGB565。
 *   src_y  : Y 平面，src_w * src_h 字节
 *   src_uv : UV 交错平面，(src_w/2)*(src_h/2)*2 字节
 *   dst    : 输出，dw*dh 个 uint16
 *
 * ★ 就地支持"1/n 整数倍抽样缩放"：src 640x480 → dst 320x240 时，
 *   每 2 个像素取 1 个、每 2 行取 1 行。整数倍时抽样比双线性插值快 10 倍
 *   且在这块小屏上肉眼看不出差别。
 */
void nv12_to_rgb565(const uint8_t *src_y, const uint8_t *src_uv,
		    int src_w, int src_h,
		    uint16_t *dst, int dw, int dh);

/**
 * NV12 → BGR24（BMP/AVI 用的字节序）。
 * 同样支持整数倍抽样缩放。
 */
void nv12_to_bgr24(const uint8_t *src_y, const uint8_t *src_uv,
		   int src_w, int src_h,
		   uint8_t *dst, int dw, int dh);

/**
 * NV12 → RGB888（每像素 R,G,B 顺序）。
 *
 * 【为什么还要一个和 bgr24 几乎一样的函数】
 *   BMP/AVI 要 BGR（Windows 的历史包袱），而 YOLO 训练时喂的是 **RGB**。
 *   拿 BGR 去推理不会报错，只是红蓝通道反了 —— 检测结果会变差但看不出来，
 *   属于最难受的那类 bug。所以宁可多一个函数把顺序写死，也不要在别处偷偷交换。
 */
void nv12_to_rgb24(const uint8_t *src_y, const uint8_t *src_uv,
		   int src_w, int src_h,
		   uint8_t *dst, int dw, int dh);

/**
 * 写 24 位 BMP。返回 0 成功。
 * rows 是 BGR24 紧凑排列（每行 dw*3 字节，不带对齐填充）。
 */
int bmp_write(const char *path, const uint8_t *bgr, int w, int h);

/* ------------------------------------------------------------------ */
/* AVI（未压缩 BGR，BI_RGB）                                          */
/* ------------------------------------------------------------------ */

/**
 * AVI 写入器。用法：
 *    avi_writer w;
 *    avi_open(&w, path, width, height, fps);   // 写头，占位
 *    avi_add_frame(&w, bgr);                   // 每帧调一次
 *    avi_close(&w);                            // 回填总帧数/索引
 *
 * ★ 为什么要"回填"：AVI 头里的 dwTotalFrames、RIFF 大小、movi 大小
 *   都必须先知道总帧数才能写。录像时不知道会录多少，
 *   所以先写 0 占位，结束时 fseek 回去改。
 *   这也是为什么必须用真实文件（能 seek），不能用管道。
 */
typedef struct {
	FILE	*fp;
	int	width, height, fps;
	uint32_t frames;
	uint32_t movi_start;	/* 'movi' LIST 的 fourcc 位置 */
	uint32_t frame_bytes;	/* 每帧 BGR 大小 */
} avi_writer;

int  avi_open(avi_writer *w, const char *path, int width, int height, int fps);
int  avi_add_frame(avi_writer *w, const uint8_t *bgr);
int  avi_close(avi_writer *w);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __CAM_IMAGE_H__ */
