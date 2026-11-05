/**
 * yolo_detect.h —— YOLOv5 目标检测的对外接口（纯 C，给 ui_yolo.c 用）
 *
 * 【整条链路的形状】
 *
 *   采集线程（cam_capture）           推理线程（本模块）            UI 线程
 *   NV12 帧 ──转RGB888──┐
 *                       └─快照→  取帧 → letterbox 到 640x640
 *                                     → rknn_run（NPU 约 80ms）
 *                                     → 解码 + NMS
 *                                     → 坐标映射回 320x240 ──seq──→ 画框
 *
 * 【为什么推理要单独开线程】
 *   单核 A7 上 rknn_run 是同步阻塞的 80ms。如果放在 lv_timer 里，
 *   这一帧的 UI 就停 80ms —— 表现出来是"手指按下去没反应、画面一顿一顿"。
 *   所以照搬相机模块的做法：后台线程算，UI 用 seq 号轮询拿结果。
 *
 * 【为什么用 seq 而不是回调】
 *   回调会跨线程碰 LVGL 对象，那是 LVGL 的单线程大忌。轮询没有这个风险。
 */
#ifndef __YOLO_DETECT_H__
#define __YOLO_DETECT_H__

#include <stdint.h>

/*
 * ★ C / C++ 混编必须加这一段，而且要加在【头文件】上。
 *   只在 .cpp 里给定义加 extern "C" 是不够的：头文件里那份声明仍然按
 *   C++ 的名字修饰走，两边对不上，报
 *       conflicting declaration of 'xxx' with 'C' linkage
 *   （或者在调用方报 undefined reference）。
 *   这个坑在 cam.h / weather.h / wifi_conn.h 上都踩过，这次又忘了一次。
 */
#ifdef __cplusplus
extern "C" {
#endif

/* 一帧最多记这么多个框。再多屏幕上也没法看，而且 NMS 之后本来就没多少 */
#define YOLO_MAX_BOX	16

/* 类别数：用的是 COCO 80 类模型 */
#define YOLO_CLASS_NUM	80

typedef struct {
	int	x, y, w, h;	/* 相对【采集图】320x240 的像素坐标 */
	int	cls;		/* 0~79 */
	float	prop;		/* 置信度 0~1 */
} yolo_box;

typedef struct {
	int		count;
	yolo_box	box[YOLO_MAX_BOX];
	uint32_t	seq;		/* 每次推理完成 +1 */
	int		infer_ms;	/* 本次推理耗时（含后处理） */
	int		src_w, src_h;	/* 采集图尺寸，坐标就是按它算的 */
} yolo_result;

/**
 * 启动检测：加载模型 + 起推理线程。
 *   model_path  : .rknn 文件（板上 /root/bin/model/yolov5.rknn）
 *   labels_path : 80 行类别名，一行一个（可传 NULL，那就只显示类别号）
 * 返回 0 成功；负数失败，原因见 yolo_last_error()。
 *
 * 注意：调用前需要 cam_start() 已经跑起来（本模块从相机拿帧）。
 */
int  yolo_start(const char *model_path, const char *labels_path);
void yolo_stop(void);
int  yolo_running(void);

/**
 * 取最新一次检测结果。
 * 返回 seq（和 out->seq 相同）；0 表示还没有结果。
 * 调用方用"seq 变了"判断要不要重画。
 */
uint32_t yolo_get(yolo_result *out);

/* 类别 id → 名字（没读懂标签文件就返回 "clsN"） */
const char *yolo_class_name(int id);

const char *yolo_status(void);
const char *yolo_last_error(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __YOLO_DETECT_H__ */
