/*
 * voice_chat.cpp —— 真语音对话链路的实现
 *
 * 做的事只有三件：
 *   1. 起一条独立线程跑 Application::Run()（它自己阻塞，不能占主线程）
 *   2. 起一条 monitor 线程，每 100ms 采一次状态，写进 atomic 快照
 *   3. 统计"进入 listening 的次数" = 开了几轮对话
 *
 * 为什么不让 UI 直接调 get_aichat_app_state()：
 *   那是在读 Application 内部成员，UI 线程和它的状态机线程并发跑，
 *   属于典型的数据竞争。哪怕现在没崩，也只是运气。
 *   所以 UI 只读我们自己维护的快照，边界干净。
 */
#include "voice_chat.h"

#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "AIchat_c_interface.h"
#include "log.h"

namespace {

/* ---- 线程安全快照：monitor 写，UI 读 ---- */
struct Snap {
    std::atomic<int>      state{VC_STATE_STARTUP};
    std::atomic<unsigned> rounds{0};
    std::atomic<int>      started{0};
    std::mutex            asr_mtx;
    char                  asr[96] = {0};
};

Snap& snap()
{
    static Snap s;   /* 函数内静态：避免全局构造顺序问题 */
    return s;
}

std::atomic<bool> g_running{false};
std::thread       g_run_thread;
std::thread       g_mon_thread;
void*             g_app = nullptr;

/* 状态名，日志用 */
const char* state_name(int s)
{
    switch (s) {
    case VC_STATE_FAULT:     return "fault";
    case VC_STATE_STARTUP:   return "startup";
    case VC_STATE_STOPPING:  return "stopping";
    case VC_STATE_IDLE:      return "Idle";
    case VC_STATE_LISTENING: return "Listening";
    case VC_STATE_THINKING:  return "Thinking";
    case VC_STATE_SPEAKING:  return "Speaking";
    default:                 return "?";
    }
}

/* ------------------------------------------------------------------ */
/* monitor：唯一的状态读取者，UI 只读它写好的快照                        */
/* ------------------------------------------------------------------ */
void monitor_loop()
{
    int prev = -1;
    int idle_miss = 0;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_app)
            continue;

        int cur = static_cast<int>(get_aichat_app_state(g_app));

        /* 轮数统计：每次进入 listening 就是"开了一轮对话"。
         * 用"上一状态 != listening"判边沿，避免 listening 期间重复计数。 */
        if (cur == VC_STATE_LISTENING && prev != VC_STATE_LISTENING) {
            unsigned n = snap().rounds.fetch_add(1) + 1;
            LOG_BUS("对话第 %u 轮开始（进入 Listening）", n);
        }
        if (cur != prev && prev != -1) {
            if (cur == VC_STATE_THINKING) {
                LOG_BUS("状态 %s → %s（已上送语音，等云端识别/回复）",
                        state_name(prev), state_name(cur));
            } else if (cur == VC_STATE_SPEAKING) {
                LOG_BUS("状态 %s → %s（正在播放回复）",
                        state_name(prev), state_name(cur));
            } else if (cur == VC_STATE_IDLE) {
                LOG_BUS("状态 %s → %s（回到待命）",
                        state_name(prev), state_name(cur));
            } else if (cur == VC_STATE_FAULT) {
                LOG_BUS("状态 %s → fault（连接异常，Startup 会重试）",
                        state_name(prev));
            }
        }
        snap().state.store(cur, std::memory_order_relaxed);

        /* 启动守望：一直卡在 startup 说明连不上网关，或者握手后迟迟没进 idle。
         * 只报警不动它 —— 状态机自己的重试逻辑在 Startup.cc 里，别抢它的活。
         * （这里原来写反了：判断的是"不在 startup"，结果正常运行也会误报。） */
        if (cur == VC_STATE_STARTUP) {
            if (++idle_miss == 300)   /* 30 秒 */
                LOG_BUS("注意：链路 30 秒仍未离开 startup，检查网关是否可达");
        } else {
            idle_miss = 0;
        }

        prev = cur;
    }
}

}  // namespace

extern "C" {

int voice_chat_start(const char* url, int port, const char* token,
                     const char* device_id, int protocol_ver)
{
    if (g_running.load())
        return 0;   /* 幂等 */

    if (!url || !token || !device_id) {
        LOG_BUS("voice_chat_start: 参数为空");
        return -1;
    }

    /* ★ snowboy 模型在 Demo 里是相对路径
     *   (third_party/snowboy/resources/common.res)，
     *   所以进程 cwd 必须是 /root/bin，否则唤醒永远不生效、
     *   而且不报任何错 —— 只是安静地永远检测不到。
     *   lvgl_app 自己的资源全用绝对路径，切 cwd 对它无影响。 */
    if (chdir("/root/bin") != 0) {
        LOG_BUS("警告：chdir(/root/bin) 失败，snowboy 模型可能加载不到");
    }

    LOG_BUS("拉起真对话链路：ws://%s:%d  协议 v%d  设备 %s",
            url, port, protocol_ver, device_id);

    g_app = create_aichat_app(url, port, token, device_id,
                              "", /* aliyun key 由网关持有，板子这边不需要 */
                              protocol_ver, 16000, 1, 40);
    if (!g_app) {
        LOG_BUS("create_aichat_app 失败");
        return -1;
    }

    snap().started.store(1);
    g_running.store(true);

    /* run_aichat_app 内部会 join 状态机线程，是阻塞调用 */
    g_run_thread = std::thread([] {
        run_aichat_app(g_app);
        LOG_BUS("对话链路线程退出");
    });

    g_mon_thread = std::thread(monitor_loop);
    return 0;
}

void voice_chat_stop(void)
{
    if (!g_running.exchange(false))
        return;

    if (g_app) {
        stop_aichat_app(g_app);   /* 往事件队列塞 to_stop */
    }
    if (g_run_thread.joinable())
        g_run_thread.join();
    if (g_mon_thread.joinable())
        g_mon_thread.join();

    if (g_app) {
        destroy_aichat_app(g_app);
        g_app = nullptr;
    }
    snap().started.store(0);
    LOG_BUS("对话链路已停止");
}

void voice_chat_get(voice_chat_snap_t* out)
{
    if (!out)
        return;
    out->state   = snap().state.load(std::memory_order_relaxed);
    out->rounds  = snap().rounds.load(std::memory_order_relaxed);
    out->started = snap().started.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(snap().asr_mtx);
    std::snprintf(out->asr, sizeof(out->asr), "%s", snap().asr);
}

/*
 * 主动唤醒一次（等价于喊唤醒词）。
 *
 * 【为什么要暴露这个】
 *   echo.pmdl 是英文个人模型，本地识别对中文口音很不稳：
 *   灵敏度高了随便说话就误唤醒，低了喊 echo 也叫不醒 —— 两头都试过了。
 *   所以留一条确定的办法：界面上的「唤醒」按钮直接调这个，
 *   走的是和语音唤醒完全相同的事件（AppEvent::wake_detected），
 *   后面的 Listening/ASR/LLM/TTS 一点区别都没有。
 */
int voice_chat_wake(void)
{
    if (!g_app)
        return -1;
    force_aichat_wake(g_app);
    LOG_BUS("手动唤醒（按钮）");
    return 0;
}

}  // extern "C"
