// audio_capture.cpp — ALSA 采集实现。头文件见 audio_capture.hpp。
#include "audio_capture.hpp"

#include "log.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

namespace sound_recv {

static constexpr const char* kTag = "capture";

AudioCapture::AudioCapture(AudioCaptureConfig cfg, EventBus& bus)
    : cfg_(std::move(cfg))
    , bus_(bus)
{
}

AudioCapture::~AudioCapture()
{
    stop();
}

bool AudioCapture::open_pcm(std::string& err)
{
    int rc = snd_pcm_open(&pcm_, cfg_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        err = "打开声卡设备 '" + cfg_.device + "' 失败: " + snd_strerror(rc)
            + " (用 arecord -l 确认设备名)";
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_, hw);

    // interleaved 读写模式
    rc = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        err = std::string("设置 access 失败: ") + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }
    // 16-bit 小端
    rc = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {
        err = std::string("设备不支持 S16_LE: ") + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // ★ 声道协商(2026-09-19 板端调试教训, 详见 AGENTS.md 坑点):
    //   ES8388 驱动声道约束最小为 2, set_channels(1) 返回 -EINVAL。
    //   旧代码忽略返回码 → hw_params 以"声道未设置"应用驱动默认 2ch →
    //   readi 每次向 mono 缓冲写入 2 倍数据 → 每 32ms 溢出 1KB 砸烂堆元数据
    //   (glibc: corrupted unsorted chunks / munmap_chunk invalid pointer)。
    //   现策略: mono 请求被拒 → 接受设备声道数, 读入后软件下混。
    hw_channels_ = cfg_.channels;
    rc = snd_pcm_hw_params_set_channels(pcm_, hw, cfg_.channels);
    if (rc < 0) {
        unsigned int chmin = 0, chmax = 0;
        snd_pcm_hw_params_get_channels_min(hw, &chmin);
        snd_pcm_hw_params_get_channels_max(hw, &chmax);
        if (chmax == 0 || chmin > chmax) {
            err = "无法获取设备声道能力: " + std::string(snd_strerror(rc));
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        hw_channels_ = chmin;
        rc = snd_pcm_hw_params_set_channels(pcm_, hw, hw_channels_);
        if (rc < 0) {
            err = "声道协商彻底失败: " + std::string(snd_strerror(rc));
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }
        SR_LOG_WARN(kTag) << "设备不支持 " << cfg_.channels << " 声道(" << chmin
                          << "~" << chmax << "), 以 " << hw_channels_
                          << " 声道采集并软件下混为 mono";
    }

    // 采样率: 要求精确 16k(dir=0), 设备不支持则报错(不做软件重采样)
    unsigned rate = cfg_.sample_rate;
    int dir = 0;
    rc = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, &dir);
    if (rc < 0 || rate != cfg_.sample_rate) {
        err = "采样率不被设备支持: 请求 " + std::to_string(cfg_.sample_rate)
            + "Hz, 设备实际 " + std::to_string(rate) + "Hz: " + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // period(一次读取的帧数)与 buffer(period × N)
    snd_pcm_uframes_t period = cfg_.period_frames;
    dir = 0;
    rc = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, &dir);
    if (rc < 0) {
        err = std::string("设置 period 失败: ") + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }
    snd_pcm_uframes_t buffer = period * cfg_.buffer_periods;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer);
    if (rc < 0) {
        err = std::string("设置 buffer 失败: ") + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    rc = snd_pcm_hw_params(pcm_, hw);
    if (rc < 0) {
        err = std::string("应用硬件参数失败: ") + snd_strerror(rc);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    // 启动方式: 数据可用即返回(readi 唤醒粒度 = 1 period)
    snd_pcm_prepare(pcm_);

    // ★ 缓冲按实际声道数分配: readi(帧数 n) 写入 n × hw_channels_ 个 int16
    raw_buf_.assign(period * hw_channels_, 0);
    SR_LOG_INFO(kTag) << "设备 " << cfg_.device << " 已打开: " << cfg_.sample_rate
                      << "Hz S16_LE " << hw_channels_ << "ch"
                      << (hw_channels_ != cfg_.channels ? "(下混为mono)" : "")
                      << ", period=" << period << "帧 buffer=" << buffer << "帧";
    return true;
}

bool AudioCapture::recover_from_error(int err, std::string& what)
{
    if (err == -EPIPE) {
        // overrun: 应用读慢导致内核缓冲满, 重武装(丢弃残存数据)
        ++xrun_count_;
        what = "overrun(XRUN)";
        return snd_pcm_prepare(pcm_) >= 0;
    }
    if (err == -ESTRPIPE) {
        // 流被挂起(如系统 suspend), 循环恢复
        what = "suspended";
        while (true) {
            int rc = snd_pcm_resume(pcm_);
            if (rc == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (rc < 0) {
                // 驱动不支持 resume → 完整重置
                return snd_pcm_prepare(pcm_) >= 0;
            }
            return true;
        }
    }
    what = snd_strerror(err);
    return false; // 未知错误, 不尝试恢复
}

// 交错多声道 int16 → mono float。帧 i 的各声道取平均后归一化。
static std::vector<float> downmix_to_mono(const int16_t* p, snd_pcm_sframes_t frames,
                                          uint32_t channels)
{
    std::vector<float> out;
    out.resize(static_cast<size_t>(frames));
    const float inv = 1.0f / (channels * 32768.0f);
    for (snd_pcm_sframes_t i = 0; i < frames; ++i) {
        const int16_t* frame = p + static_cast<size_t>(i) * channels;
        int32_t acc = 0;
        for (uint32_t c = 0; c < channels; ++c) {
            acc += frame[c];
        }
        out[static_cast<size_t>(i)] = static_cast<float>(acc) * inv;
    }
    return out;
}

void AudioCapture::run()
{
    const size_t frames_per_read = raw_buf_.size() / hw_channels_;
    uint32_t consecutive_errors = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, raw_buf_.data(),
                                            static_cast<snd_pcm_uframes_t>(frames_per_read));
        if (n < 0) {
            std::string what;
            if (recover_from_error(static_cast<int>(n), what)) {
                ++consecutive_errors;
                SR_LOG_WARN(kTag) << "读取中断已恢复: " << what << " (xrun_count="
                                  << xrun_count_.load() << ")";
            } else {
                ++consecutive_errors;
                SR_LOG_ERROR(kTag) << "读取失败且无法恢复: " << what;
            }
            if (consecutive_errors >= cfg_.max_consecutive_errors) {
                bus_.publish(make_event(EventType::Error,
                                        ErrorData{"音频采集连续失败 " + std::to_string(consecutive_errors)
                                                  + " 次, 采集线程退出"}));
                SR_LOG_ERROR(kTag) << "连续错误达到上限, 退出采集线程";
                return;
            }
            continue;
        }

        if (n > 0) {
            consecutive_errors = 0;
            // 按实际声道数转 mono float, move 发布(指针转移, 无深拷贝)
            std::vector<float> mono = (hw_channels_ == 1)
                ? pcm_s16_to_float(raw_buf_.data(), static_cast<size_t>(n))
                : downmix_to_mono(raw_buf_.data(), n, hw_channels_);
            bus_.publish(make_event(EventType::AudioChunk, AudioChunkData{std::move(mono)}));
        }
    }
}

bool AudioCapture::start(std::string* err)
{
    if (thread_.joinable()) {
        return true; // 已启动
    }
    std::string e;
    if (!open_pcm(e)) {
        if (err) {
            *err = e;
        }
        SR_LOG_ERROR(kTag) << e;
        return false;
    }
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
    return true;
}

void AudioCapture::stop()
{
    if (!thread_.joinable()) {
        return;
    }
    stop_flag_.store(true, std::memory_order_relaxed);
    thread_.join(); // readi 周期 32ms, 最长一个周期内感知停止位
    if (pcm_) {
        snd_pcm_drop(pcm_); // 丢弃未读数据立即停机(不再需要数据)
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    SR_LOG_INFO(kTag) << "采集已停止 (xrun_count=" << xrun_count_.load() << ")";
}

} // namespace sound_recv
