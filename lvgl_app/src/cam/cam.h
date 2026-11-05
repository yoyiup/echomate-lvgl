/**
 * cam.h —— 摄像头采集的对外接口（纯 C，零依赖）
 *
 * 【为什么要有这一层】
 * LVGL 是单线程的，而采集是"等帧 → 处理 → 等下一帧"的阻塞循环。
 * 如果把采集塞进 lv_timer 里，一次 DQBUF 卡住整个 UI 就冻住了。
 *
 * 所以结构是：
 *
 *   采集线程（本模块）                      LVGL 主线程
 *   poll(video) → DQBUF                    lv_timer 每 100ms
 *        ↓                                      ↓
 *   转成 RGB565 写进双缓冲  ────快照────>  cam_preview() 拷走画到 canvas
 *        ↓
 *   拍照/录像（收到请求时）
 *
 * 【双缓冲 + 序号】是跨线程传图像的正确姿势：
 *   采集线程只写"后备缓冲"，写完才把序号 +1；
 *   UI 线程用序号判断"有没有新帧"，有就整块 memcpy 走。
 *   这样 UI 永远不会读到写了一半的帧（撕裂）。
 */

#ifndef __CAM_H__
#define __CAM_H__

#include <stdint.h>
#include <stdbool.h>

/*
 * ★ C / C++ 混编必须加。
 *
 *   实现（cam_capture.cpp）里这些函数是 extern "C" 的，
 *   如果头文件不声明，从 .cpp 里调用会被 C++ 当成 C++ 符号去找
 *   （_Z9cam_startPKcS0_ 这种），链接报 undefined reference。
 *
 *   反过来也成立：纯 C 文件 include 本头时，这段被 #ifdef 屏蔽，
 *   一点影响都没有。
 */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 预览尺寸。
 *
 * ★★★ 这个宏必须和 ui_camera.c 里的 canvas 尺寸【完全一致】，否则
 *   cam_preview() 会按 CAM_PV_H 拷数据，而 canvas 缓冲是按自己的 PV_H
 *   分配的 —— 拷多了就是【堆溢出】，踩坏 LVGL 的对象树。
 *
 *   实际踩过：CAM_PV_H 曾是 240，canvas 是 320x160（PV_H=160），
 *   每帧越界写 320*(240-160)*2 = 51200 字节。症状是画面下半部分异常
 *   + 跑十几秒后进程无任何报错就消失（被踩坏的内存导致的崩溃/死循环）。
 *
 *   现在 ui_camera.c 直接用这两个宏，不再自己定义 PV_H。
 */
#define CAM_PV_W	320
#define CAM_PV_H	160

/*
 * 采集分辨率。
 *
 * ★ 用 320x240 而不是 640x480，两个原因：
 *   ① 内存：这块板子只有 54MB。4 个 buffer 在 640x480 时要
 *      4×460800 = 1.8MB，320x240 只要 4×115200 = 460KB，省 1.4MB。
 *      而 rkaiq 的 3A 要吃掉十几 MB，内存非常紧。
 *   ② 无用功：预览区只有 320x160，采 640x480 再缩小一半，
 *      多花 ISP 带宽和 CPU 却看不出区别。
 *
 * ★ 320x240 是 4:3，预览区 320x160 是 2:1 —— 比例不同！
 *   所以预览时【垂直方向居中裁剪】而不是压缩：
 *   取中间 320x160 行（上下各裁 40 行），1:1 像素映射，
 *   既不拉伸变形，也不需要重采样。
 */
#define CAM_CAP_W	320
#define CAM_CAP_H	240

/* 采集帧率。跟 sensor 的 25fps 对齐，不额外降帧，让 3A 收敛得快些 */
#define CAM_FPS		25

/* ---------------- 生命周期 ---------------- */

/**
 * 启动采集。成功返回 0。
 *   video_dev : "/dev/video0"（rkisp mainpath）等
 *   iq_dir    : iq 参数目录，传 NULL 用 "/tmp/cam/iqfiles"
 *
 * 内部会：起 rkaiq 3A（失败则降级为"无 3A"，图像可能偏暗但能出图）
 *        + 起采集线程。
 */
int  cam_start(const char *video_dev, const char *iq_dir);
void cam_stop(void);
bool cam_running(void);

/* ---------------- 预览 ---------------- */

/**
 * 拷一帧 RGB565 到 dst。
 *   dst       : 目标缓冲
 *   dst_bytes : dst 的字节数 —— ★ 必填，内部校验，不够就拒绝拷贝。
 *               （加这个参数就是因为之前没有校验，越界写 50KB 把堆踩了）
 * 返回帧序号：>0 表示有新帧；0 表示没有可用帧或缓冲太小。
 */
uint32_t cam_preview(uint16_t *dst, int dst_bytes);

/* ---------------- YOLO 用的整幅 RGB888 ---------------- */

/**
 * 打开/关闭整幅 RGB888 通路（1 开 0 关）。
 * 关着的时候采集线程完全不做这件事，所以平时没有额外开销。
 * 进 YOLO 页面前打开，离开时关掉。
 */
void cam_rgb_enable(int on);

/**
 * 拷走最新一帧整幅 RGB888（尺寸见 cam_capture_size）。
 *   dst_bytes 必须 >= w*h*3，否则返回 0。
 * 返回帧序号：>0 有新帧，0 表示没开/还没出帧。
 */
uint32_t cam_frame_rgb(uint8_t *dst, int dst_bytes);

/** 取采集尺寸（当前 320x240），给上层分配缓冲用。返回 0 恒成功。 */
int cam_capture_size(int *w, int *h);

/* ---------------- 拍照 ---------------- */

/**
 * 拍一张 BMP。异步：把请求交给采集线程，下一帧到达时写文件。
 * 返回 0 = 请求已接受。-1 = 队列已有未完成的拍照请求。
 */
int  cam_photo(const char *path);

/* 已成功保存的张数（UI 显示用） */
uint32_t cam_photo_count(void);

/* 最近一张的路径（没拍过返回空串） */
const char *cam_last_photo(void);

/* ---------------- 录像 ---------------- */

/**
 * 开始录像（AVI，未压缩 BGR24，25fps 输入按 fps 抽帧）。
 *   path : 输出文件
 *   fps  : 1~10，实际写入帧率
 *
 * 超过 CAM_REC_MAX_SEC 秒自动停（防止把 /tmp 写爆）。
 */
int  cam_record_start(const char *path, int fps);
int  cam_record_stop(void);
bool cam_recording(void);

/* 已录秒数（UI 显示用） */
int  cam_record_seconds(void);

/* ---------------- 其他 ---------------- */

const char *cam_status(void);	/* 一行状态串，给 UI 显示 */
const char *cam_last_error(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __CAM_H__ */
