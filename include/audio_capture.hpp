// audio_capture.hpp — ALSA PCM 采集线程封装。
//
// 职责: 独立线程循环 snd_pcm_readi 读取 16k/S16_LE/mono PCM, 就地转 float,
// 以 AudioChunk 事件发布到总线。队列满时由 BlockingQueue 丢最旧, 采集线程永不阻塞。
//
// XRUN 恢复(design.md 6.2 / AGENTS.md 关键坑):
//   -EPIPE    (overrun)  → snd_pcm_prepare() 重武装, 丢弃内核缓冲残存数据
//   -ESTRPIPE (挂起暂停) → 循环 snd_pcm_resume() 直到恢复
//   连续失败超过 max_consecutive_errors → 发布 Error 事件并退出线程(不崩溃)
#ifndef SOUND_RECV_AUDIO_CAPTURE_HPP
#define SOUND_RECV_AUDIO_CAPTURE_HPP

#include "event_bus.hpp"
#include "types.hpp"

#include <alsa/asoundlib.h>
#include <atomic>
#include <string>
#include <thread>

namespace sound_recv {

class AudioCapture {
public:
    /// bus: 事件总线; cfg: 采集参数(默认值见 types.hpp)。
    AudioCapture(AudioCaptureConfig cfg, EventBus& bus);
    ~AudioCapture(); // RAII: 自动 stop()

    /// 打开 PCM 设备并拉起采集线程。失败时 err 填入人话诊断。
    bool start(std::string* err = nullptr);

    /// 置停止位 → join 线程 → 关闭设备。幂等。
    void stop();

    /// 运行期统计: XRUN 恢复次数(性能监控)。
    uint64_t xrun_count() const { return xrun_count_.load(std::memory_order_relaxed); }

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

private:
    void run();                    ///< 采集线程主循环
    bool open_pcm(std::string& err);
    bool recover_from_error(int err, std::string& what);

    AudioCaptureConfig cfg_;
    EventBus& bus_;
    snd_pcm_t* pcm_ = nullptr;
    uint32_t hw_channels_ = 1;       ///< 驱动实际运行的声道数(可能与请求不同, 见 open_pcm)
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<uint64_t> xrun_count_{0};
    std::vector<int16_t> raw_buf_;   ///< 复用的 int16 中转缓冲(按实际声道分配, 避免每帧堆分配)
};

} // namespace sound_recv

#endif // SOUND_RECV_AUDIO_CAPTURE_HPP
