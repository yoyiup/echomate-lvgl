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
 * 默认拍照、AVI 和 H.265 使用 640x480；H.265 可切换到1280x960。
 * 屏幕预览仍为 320x160，
 * 转换时横纵各抽样 1/2，并从 4:3 原图中央裁成 2:1，避免拉伸。
 * 4 个 NV12 采集缓冲合计约 1.8MB；修改时要留意 54MB 内存上限。
 */
#define CAM_CAP_W	640
#define CAM_CAP_H	480

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

/** 停止状态下设置下一次启动的采集尺寸；仅接受640x480或1280x960。 */
int cam_set_capture_size(int w, int h);

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
 * 请求并拷走下一帧整幅 RGB888（尺寸见 cam_capture_size）。
 *   dst_bytes 必须 >= w*h*3，否则返回 0。
 * 返回帧序号：>0 有尚未读取的新帧，0 表示没开或新帧尚未到达。
 */
uint32_t cam_frame_rgb(uint8_t *dst, int dst_bytes);

/** 取驱动最终接受的当前采集尺寸，给上层分配缓冲用。返回0恒成功。 */
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
 * 开始录像（AVI，未压缩 BGR24，从实际采集帧中按时间抽帧）。
 *   path : 输出文件
 *   fps  : 1~10，实际写入帧率
 *
 * 超过 CAM_REC_MAX_SEC 秒自动停（防止把 /tmp 写爆）。
 */
int  cam_record_start(const char *path, int fps);
/** 第二种录像格式：硬件 H.265 编码，停止时封装为 MP4。 */
int  cam_record_start_h265(const char *path);
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
