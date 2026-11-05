/*
 * voice_chat.h —— 真语音对话链路（WS + Opus + ASR + TTS）的 C 接口封装
 *
 * 【为什么要这一层】
 * AIChat Demo 的 Client 本身就是一个完整应用：它自带 snowboy 唤醒、
 * 自己的状态机、自己的麦克风和喇叭。直接把它当成"库"来用会遇到三个问题：
 *
 *   1. run_aichat_app() 是【阻塞】的（内部 join 状态机线程），
 *      必须放到独立线程里跑，不能卡住 LVGL 主循环。
 *   2. 它的状态是内部枚举，UI 直接读会跨线程碰脏数据 ——
 *      所以这里只暴露一份 atomic 快照，UI 线程爱什么时候读都行。
 *   3. snowboy 模型在 Demo 里写的是【相对路径】
 *      (third_party/snowboy/resources/common.res)，
 *      进程 cwd 必须是 /root/bin，否则模型加载失败、永远唤不醒。
 *      这一层负责切 cwd，上层不用关心。
 *
 * 【麦克风独占：最重要的约束】
 * 板子的 default 输入设备只能被一个进程/一路流打开。
 * 所以下面两条路【只能选一条】，不能同时开：
 *   A. 本文件（Application 内置 snowboy + 对话 + TTS 播放）—— 默认
 *   B. snowboy_wake.cpp（我们自己写的适配器）—— 只在 WAKE_MODE=own 时用
 * 同时开会表现为第二路 Pa_OpenStream 失败，或者干脆谁都收不到声音。
 */
#ifndef VOICE_CHAT_H
#define VOICE_CHAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 和 ChatState 枚举一一对应（见 AIchat_c_interface.h） */
enum {
    VC_STATE_FAULT     = 0,
    VC_STATE_STARTUP   = 1,
    VC_STATE_STOPPING  = 2,
    VC_STATE_IDLE      = 3,
    VC_STATE_LISTENING = 4,
    VC_STATE_THINKING  = 5,
    VC_STATE_SPEAKING  = 6,
};

typedef struct {
    int      state;        /* 上面那个枚举 */
    unsigned rounds;       /* 开过几轮对话（进入 listening 的次数） */
    int      started;      /* 链路是否已拉起 */
    char     asr[96];      /* 最近一次 ASR 识别结果，空串表示还没有 */
} voice_chat_snap_t;

/*
 * 起链路。返回 0 成功。
 * 参数来自板上 /root/bin/system_para.conf：
 *   url=172.32.0.1  port=8000  token=123456
 *   device_id=00:11:22:33:44:55  protocol_ver=2
 *
 * ★ 内部会 chdir("/root/bin")，因为 snowboy 模型是相对路径。
 *   调用前请确保 lvgl_app 自己的资源都用绝对路径（已确认）。
 */
int  voice_chat_start(const char* url, int port, const char* token,
                      const char* device_id, int protocol_ver);

/* 停链路并回收线程。可重复调用。 */
void voice_chat_stop(void);

/* 读快照。任何线程、任意频率都可以调。 */
void voice_chat_get(voice_chat_snap_t* out);

/*
 * 主动唤醒一次，等价于喊唤醒词。
 * 走和语音唤醒完全相同的事件链（后续 Listening/ASR/LLM/TTS 无区别）。
 * 用途：唤醒词识别不出来的兜底 —— 界面上给一个「唤醒」按钮。
 * 返回 0 成功；-1 链路没起。
 */
int  voice_chat_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_CHAT_H */
