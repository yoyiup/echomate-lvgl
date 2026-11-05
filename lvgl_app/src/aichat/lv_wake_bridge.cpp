#include "lv_wake_bridge.h"

#include "lvgl/lvgl.h"

#include "log.h"
#include "mock_detector.h"
#include "snowboy_wake.h"
#include "supervisor.h"
#include "voice_chat.h"
#include "wake_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using Mode = MockWakeDetector::Mode;

static std::unique_ptr<WakeRuntime> g_rt;
static std::unique_ptr<Supervisor>  g_sup;
static lv_obj_t*                    g_lbl   = nullptr;
static lv_timer_t*                  g_timer = nullptr;
static char                         g_last[96] = {0};

/* 走的是哪条路？
 *   true  = 真对话链路（Application 内置 snowboy + WS + Opus + TTS）
 *   false = 我们自己写的唤醒框架（演示/去抖验证用）
 *
 * ★ 这两条路【绝对不能同时开】：它们都要打开 default 输入设备，
 *   而板子的 ALSA 录音设备同一时刻只能有一路流。
 *   同时开的表现是第二路 Pa_OpenStream 失败，或者谁都收不到声音。 */
static bool g_real = false;

/* 状态名转短标签。屏幕只有 320×240，且默认字体只有 ASCII。 */
static const char* vc_short(int s) {
    switch (s) {
    case VC_STATE_FAULT:     return "Fault";
    case VC_STATE_STARTUP:   return "Startup";
    case VC_STATE_STOPPING:  return "Stopping";
    case VC_STATE_IDLE:      return "Idle";
    case VC_STATE_LISTENING: return "Listen";
    case VC_STATE_THINKING:  return "Think";
    case VC_STATE_SPEAKING:  return "Speak";
    default:                 return "?";
    }
}

// ★ 只有这个回调（lv_timer，主线程）能碰 lv_obj_*
static void ui_refresh_cb(lv_timer_t*) {
    if (!g_lbl) return;

    char buf[96];

    // ★ 必须纯 ASCII：LVGL 默认字体（montserrat_14）只有 ASCII 字形，
    //   写中文会显示成空白/方块。
    if (g_real) {
        voice_chat_snap_t s;
        voice_chat_get(&s);
        std::snprintf(buf, sizeof(buf), "chat:%s r=%u%s",
                      vc_short(s.state), s.rounds,
                      s.started ? "" : " (off)");
    } else {
        if (!g_rt) return;
        std::snprintf(buf, sizeof(buf), "wake:%s r=%llu x=%llu%s",
                      to_str(g_rt->state()),
                      static_cast<unsigned long long>(g_rt->dialogue_rounds()),
                      static_cast<unsigned long long>(g_rt->rejected()),
                      (g_sup && g_sup->degraded()) ? " degraded" : "");
    }

    // 内容没变就不刷：这块屏整屏刷新 28ms、SPI 带宽有限，省一次是一次
    if (std::strcmp(buf, g_last) == 0) return;
    std::strcpy(g_last, buf);
    lv_label_set_text(g_lbl, buf);
}

void wake_bridge_init(void) {
    if (g_rt || g_real) return;   // 幂等

    /* 模式选择
     *   （默认）      真对话链路
     *   WAKE_MODE=own 我们自己写的 snowboy 适配器（不连云端，只推状态机）
     *   WAKE_MODE=mock|storm  模拟驱动，验证去抖（不读麦）
     */
    const char* m = std::getenv("WAKE_MODE");
    const bool use_mock =
        (m && (std::strcmp(m, "mock") == 0 || std::strcmp(m, "storm") == 0));
    const bool use_own = (m && std::strcmp(m, "own") == 0);

    if (!use_mock && !use_own) {
        /* ---- 真实链路：只登记"要用哪条路"，不自动拉起 ----
         *
         * 原来是 init 里直接 voice_chat_start()，开机就连云端、占着麦克风。
         * 现在改成"界面入口"模式：由「AI 对话」页的按钮显式启动，
         * 好处是——不开这个页面就不占麦、不连网、不耗电；
         * 代价是——唤醒词只在页面打开时有效（麦克风是独占资源，
         * 两路同时开会互相抢，见 g_real 上面那段注释）。
         */
        LOG_APP("wake bridge: 真实链路待命（进「AI 对话」页再启动）");
        return;
    }

    /* ---- 演示链路：我们自己的框架 ---- */
    g_rt = std::make_unique<WakeRuntime>(/*cooldown_ms=*/1000,
                                         /*queue_cap=*/8,
                                         /*score_pct=*/50);
    g_rt->start();

    Supervisor::Factory factory = [use_mock, m]() -> std::unique_ptr<IWakeDetector> {
        if (use_mock) {
            Mode mode = (m && std::strcmp(m, "storm") == 0) ? Mode::Storm
                                                            : Mode::Normal;
            return std::make_unique<MockWakeDetector>(mode, 0);
        }
        // WAKE_MODE=own：真 snowboy，但只推到我们的状态机，不连云端
        return std::make_unique<SnowboyWake>(
            "/root/bin/third_party/snowboy/resources/common.res",
            "/root/bin/third_party/snowboy/resources/models/echo.pmdl",
            0.85f, 2.0f);
    };

    WakeConfig cfg;
    g_sup = std::make_unique<Supervisor>(factory, cfg, *g_rt,
                                         /*watchdog_ms=*/1500,
                                         /*max_retry=*/3);
    if (!g_sup->bring_up())
        LOG_APP("wake bridge: 语音唤醒不可用，等待按键唤醒");
    else
        LOG_APP("wake bridge: 语音唤醒就绪");
    g_sup->start_watchdog();
}

void wake_bridge_attach(void* label) {
    if (!label) return;
    g_lbl = static_cast<lv_obj_t*>(label);
    if (!g_timer)
        g_timer = lv_timer_create(ui_refresh_cb, 100, nullptr);
}

void wake_bridge_feed(const int16_t* pcm, size_t samples) {
    // 真实链路自己开麦、自己检测，不吃外面的 PCM（否则会双重检测）
    if (!g_real && g_sup) g_sup->feed_frame(pcm, samples);
}

void wake_bridge_press(void) {
    // 按键和语音唤醒走的是同一个事件、同一个状态机 —— 降级不等于另写一套逻辑
    if (!g_real && g_rt) g_rt->post(AppEvent::WakeDetected);
}

const char* wake_bridge_status(void) {
    static char s[96];
    if (g_real) {
        voice_chat_snap_t st;
        voice_chat_get(&st);
        std::snprintf(s, sizeof(s), "%s rounds=%u%s",
                      vc_short(st.state), st.rounds,
                      st.started ? "" : " off");
        return s;
    }
    if (!g_rt) return "not init";
    std::snprintf(s, sizeof(s), "%s rounds=%llu rejected=%llu%s",
                  to_str(g_rt->state()),
                  static_cast<unsigned long long>(g_rt->dialogue_rounds()),
                  static_cast<unsigned long long>(g_rt->rejected()),
                  (g_sup && g_sup->degraded()) ? " degraded" : "");
    return s;
}

void wake_bridge_deinit(void) {
    if (g_timer) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    g_lbl = nullptr;
    std::memset(g_last, 0, sizeof(g_last));

    if (g_real) {
        voice_chat_stop();
        g_real = false;
        LOG_APP("wake bridge 已关闭（真实链路）");
        return;
    }

    // 顺序有讲究：先停 SDK（不再产生新事件），再停业务线程
    if (g_sup) g_sup->stop();
    if (g_rt)  g_rt->stop();
    g_sup.reset();
    g_rt.reset();
    LOG_APP("wake bridge 已关闭");
}

/* ------------------------------------------------------------------ */
/* 对话链路的显式启停（给「AI 对话」页面用）                          */
/* ------------------------------------------------------------------ */
/*
 * 【为什么把启停放到这里，而不是让页面直接调 voice_chat_start】
 *   voice_chat_* 是底层封装，它不知道"当前是不是走演示模式(mock/own)"。
 *   如果页面直接调，会出现"演示模式下点按钮又把真链路拉起来"——
 *   两路同时抢麦克风，表现为谁都收不到声音（见 g_real 上面的说明）。
 *   所以统一从这里走：只有 init 判定为"真实链路模式"时才允许启动。
 */

int wake_bridge_chat_start(void) {
    if (g_real) return 0;          /* 已经在跑，幂等 */

    const char* m = std::getenv("WAKE_MODE");
    const bool demo_mode = (m && (std::strcmp(m, "mock") == 0 ||
                                  std::strcmp(m, "storm") == 0 ||
                                  std::strcmp(m, "own") == 0));
    if (demo_mode) {
        LOG_APP("wake bridge: 处于演示模式(WAKE_MODE=%s)，不启动真实链路", m);
        return -1;
    }

    /*
     * ★ 唤醒方式：默认只认唤醒词 "echo"
     *
     *   客户端 Idle 态里有一条"能量兜底"：只要麦克风 rms >= SNOWBOY_WAKE_RMS
     *   （默认 400）就唤醒，根本不管说的是什么词。所以不喊 echo、只是
     *   说话声音大一点，它也会自己接话 —— 这就是"没喊它却响起来"的原因。
     *
     *   这里在起链路之前把它置 0（= 关闭能量兜底，只走 snowboy 的 echo.pmdl）。
     *   用 setenv(..., overwrite=0)：外部要是显式设了值（调参时），以外部为准，
     *   不会被这里覆盖掉。
     *
     *   想恢复"够响就醒"：SNOWBOY_WAKE_RMS=400 ./lvgl_app
     *   一直唤不醒：SNOWBOY_SENSITIVITY=0.95 或 SNOWBOY_AUDIO_GAIN=3
     */
    setenv("SNOWBOY_WAKE_RMS", "0", 0);

    /*
     * ★ 灵敏度 / 增益：默认值必须收紧，否则"随便说句话也醒"
     *
     *   客户端原来默认 sensitivity=0.85、audio_gain=2.0。这两个值对
     *   echo.pmdl（英文个人模型）来说太激进：
     *     ① 0.85 的灵敏度会让"听起来有点像"的词也过阈值；
     *     ② gain=2.0 是把麦克风信号先放大 2 倍再喂检测器，信噪比提高的同时
     *        把环境噪声也一起抬上去，误触发明显变多。
     *   实测（2026-11-01）：能量兜底已经关掉（`Wake by energy` 0 次），
     *   但 20 秒内 snowboy 自己误唤了 4 次 —— 全是普通话说话触发的。
     *
     *   两头都试过了：0.85/2.0 二十秒误唤 4 次；0.50/1.0 喊 echo 也叫不醒。
     *   现在取中间 0.65 / 1.2。
     *     · gain=1.0 与模型训练条件一致（训练时不做前置放大）；
     *     · 0.50 是官方示例常用的保守值，宁可多喊两遍也不要乱醒。
     *   还能继续调：SNOWBOY_SENSITIVITY=0.8 SNOWBOY_AUDIO_GAIN=1.5 ./lvgl_app
     */
    setenv("SNOWBOY_SENSITIVITY", "0.65", 0);
    setenv("SNOWBOY_AUDIO_GAIN", "1.2", 0);

    if (voice_chat_start("172.32.0.1", 8000, "123456",
                         "00:11:22:33:44:55", /*protocol=*/2) != 0) {
        LOG_APP("wake bridge: 对话链路拉起失败");
        return -1;
    }

    g_real = true;
    LOG_APP("wake bridge: 对话链路已拉起");
    return 0;
}

void wake_bridge_chat_stop(void) {
    if (!g_real) return;

    voice_chat_stop();
    g_real = false;
    LOG_APP("wake bridge: 对话链路已停止");
}

int wake_bridge_chat_running(void) {
    return g_real ? 1 : 0;
}

int wake_bridge_chat_wake(void) {
    return voice_chat_wake();
}
