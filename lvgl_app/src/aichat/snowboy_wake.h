// 真实 snowboy 适配器 —— 把"AIChat 里的那套"收进 IWakeDetector
//
// 职责：
//   ① init：构造 SnowboyDetect（common.res + echo.pmdl）
//   ② start：PortAudio 开麦 → 录音回调只入队 → 内部线程跑检测
//   ③ 命中：回调 cb_（投递给 WakeRuntime，去抖/状态机在那边）
//   ④ self_capture()=true：告诉业务层"我自己开麦"，别重复启动录音流
//
// 为什么检测要独立线程、而不是在 PortAudio 回调里直接跑：
//   Pa 回调是实时音频线程，必须立刻返回（否则爆音/丢帧）；
//   SnowboyDetectRunDetection 是 CPU 密集的。所以回调只入队。
#pragma once

#include "i_wake_detector.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct SnowboyDetect;

class SnowboyWake : public IWakeDetector {
public:
    SnowboyWake(std::string res_path, std::string model_path,
                float sensitivity = 0.85f, float gain = 2.0f);
    ~SnowboyWake() override;

    bool init(const WakeConfig& cfg) override;
    bool start() override;
    void stop() override;
    void deinit() override;

    bool feed(const int16_t* pcm, size_t samples) override;

    // ★ 我们自己开麦（PortAudio），业务层不要再启动录音流
    bool self_capture() const override { return true; }

    void set_callback(WakeCb cb) override { cb_ = std::move(cb); }
    uint64_t heartbeat() const override { return beat_.load(std::memory_order_relaxed); }
    const char* name() const override { return "Snowboy"; }

    // 由 PortAudio 录音回调调用（实时线程，只做入队）
    void push_pcm(const int16_t* pcm, size_t samples);

private:
    void worker();

    std::string res_, model_;
    float       sens_, gain_;

    SnowboyDetect* det_    = nullptr;
    void*          stream_ = nullptr;   // PaStream*，用 void* 避免头文件外泄

    std::thread               th_;
    std::mutex                mu_;
    std::condition_variable   cv_;
    std::queue<std::vector<int16_t>> q_;

    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> beat_{0};
    WakeCb                cb_;
};
