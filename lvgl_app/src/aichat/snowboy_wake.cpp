#include "snowboy_wake.h"
#include "log.h"

#include <portaudio.h>
#include <snowboy-detect-c-wrapper.h>

#include <cstdio>

// PortAudio 录音回调（实时线程）：只入队，绝不在这里跑检测
static int pa_record_cb(const void* in, void*, unsigned long frames,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
                        void* user) {
    auto* self = static_cast<SnowboyWake*>(user);
    const int16_t* pcm = static_cast<const int16_t*>(in);
    if (pcm && frames)
        self->push_pcm(pcm, frames);
    return paContinue;
}

SnowboyWake::SnowboyWake(std::string res, std::string model,
                         float sens, float gain)
    : res_(std::move(res)), model_(std::move(model)),
      sens_(sens), gain_(gain) {}

SnowboyWake::~SnowboyWake() {
    stop();
    deinit();
}

bool SnowboyWake::init(const WakeConfig&) {
    det_ = SnowboyDetectConstructor(res_.c_str(), model_.c_str());
    if (!det_) {
        LOG_SDK("Snowboy 构造失败（检查资源是否存在）: %s | %s",
                res_.c_str(), model_.c_str());
        return false;
    }

    char s[16];
    std::snprintf(s, sizeof(s), "%.2f", sens_);
    SnowboyDetectSetSensitivity(det_, s);
    SnowboyDetectSetAudioGain(det_, gain_);
    SnowboyDetectApplyFrontend(det_, false);

    LOG_SDK("Snowboy 就绪: hotword=%d %dHz %dch %dbit (sens=%.2f gain=%.1f)",
            SnowboyDetectNumHotwords(det_),
            SnowboyDetectSampleRate(det_),
            SnowboyDetectNumChannels(det_),
            SnowboyDetectBitsPerSample(det_),
            sens_, gain_);
    return true;
}

bool SnowboyWake::start() {
    if (running_.exchange(true))
        return true;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        LOG_SDK("Pa_Initialize 失败: %s", Pa_GetErrorText(err));
        running_.store(false);
        return false;
    }

    PaStreamParameters in{};
    int dev = Pa_GetDefaultInputDevice();
    if (dev == paNoDevice) {
        LOG_SDK("没有默认录音设备");
        Pa_Terminate();
        running_.store(false);
        return false;
    }
    in.device = dev;
    in.channelCount = 1;
    in.sampleFormat = paInt16;
    in.suggestedLatency = Pa_GetDeviceInfo(dev)->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    PaStream* st = nullptr;
    err = Pa_OpenStream(&st, &in, nullptr,
                        /*sampleRate=*/16000, /*framesPerBuffer=*/640,
                        paClipOff, pa_record_cb, this);
    if (err != paNoError) {
        LOG_SDK("Pa_OpenStream 失败: %s", Pa_GetErrorText(err));
        Pa_Terminate();
        running_.store(false);
        return false;
    }
    stream_ = st;

    err = Pa_StartStream(st);
    if (err != paNoError) {
        LOG_SDK("Pa_StartStream 失败: %s", Pa_GetErrorText(err));
        Pa_CloseStream(st);
        stream_ = nullptr;
        Pa_Terminate();
        running_.store(false);
        return false;
    }

    th_ = std::thread(&SnowboyWake::worker, this);
    LOG_SDK("麦克风已打开（%s），开始检测", Pa_GetDeviceInfo(dev)->name);
    return true;
}

void SnowboyWake::stop() {
    if (!running_.exchange(false))
        return;

    cv_.notify_all();

    if (stream_) {
        Pa_StopStream(static_cast<PaStream*>(stream_));
        Pa_CloseStream(static_cast<PaStream*>(stream_));
        stream_ = nullptr;
    }
    if (th_.joinable())
        th_.join();
    Pa_Terminate();

    {
        std::lock_guard<std::mutex> lk(mu_);
        std::queue<std::vector<int16_t>> empty;
        std::swap(q_, empty);
    }
    LOG_SDK("麦克风已关闭");
}

void SnowboyWake::deinit() {
    if (det_) {
        SnowboyDetectDestructor(det_);
        det_ = nullptr;
    }
    LOG_SDK("Snowboy 已销毁");
}

bool SnowboyWake::feed(const int16_t* pcm, size_t samples) {
    if (!running_.load())
        return false;
    push_pcm(pcm, samples);
    return true;
}

void SnowboyWake::push_pcm(const int16_t* pcm, size_t samples) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.size() >= 32)      // 有界：积压就丢最旧的，绝不无限涨
        q_.pop();
    q_.emplace(pcm, pcm + samples);
    cv_.notify_one();
}

void SnowboyWake::worker() {
    std::vector<int16_t> frame;
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !q_.empty() || !running_.load(); });
            if (!running_.load())
                break;
            frame = std::move(q_.front());
            q_.pop();
        }
        if (!det_ || frame.empty())
            continue;

        int r = SnowboyDetectRunDetection(det_, frame.data(),
                                          static_cast<int>(frame.size()), false);
        beat_.fetch_add(1, std::memory_order_relaxed);
        if (r > 0) {
            LOG_SDK("snowboy 命中！keywordId=%d", r);
            if (cb_)
                cb_(WakeHit{r, 0.99f, now_mono_ms()});
        }
    }
}
