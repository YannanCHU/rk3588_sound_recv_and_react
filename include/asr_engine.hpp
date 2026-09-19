// asr_engine.hpp — VAD + ASR 组合引擎(sherpa-onnx C-API, 纯 CPU 推理)。
//
// 职责: 消费 AudioChunk 事件 → 喂入 Silero VAD 状态机 → 一句话结束
// (静音超 min_silence_duration)取出完整分片 → SenseVoice int8 离线解码
// → 发布 TextRecognized 事件。空识别结果按 design.md 6.2 优雅丢弃。
//
// 线程模型: 独立工作线程; 解码(SenseVoice 前向)期间不再消费新 chunk,
// 由采集侧 BlockingQueue 丢最旧保实时 —— VAD 内部环形缓冲(30s)保证连续性。
//
// 关停语义: stop() 会 Flush 尾音, 把最后一段未触发静音超时的语音也解码出来,
// 保证用户"说完最后一个字就按 Ctrl+C"不丢内容。
#ifndef SOUND_RECV_ASR_ENGINE_HPP
#define SOUND_RECV_ASR_ENGINE_HPP

#include "event_bus.hpp"
#include "types.hpp"

#include <atomic>
#include <future>
#include <string>
#include <thread>

// sherpa-onnx C-API 的不透明句柄前向声明(避免头文件泄漏到公共接口)
typedef struct SherpaOnnxVoiceActivityDetector SherpaOnnxVoiceActivityDetector;
typedef struct SherpaOnnxOfflineRecognizer SherpaOnnxOfflineRecognizer;
typedef struct SherpaOnnxOfflineStream SherpaOnnxOfflineStream;

namespace sound_recv {

class AsrEngine {
public:
    AsrEngine(AsrConfig cfg, EventBus& bus);
    ~AsrEngine(); // RAII: 自动 stop() 并释放全部模型资源

    /// 初始化 VAD + Recognizer(加载 onnx 模型, 耗时秒级)并拉起工作线程。
    /// 模型路径不存在/不合法 → 返回 false, err 为人话诊断。
    bool start(std::string* err = nullptr);

    /// 置停止位 → flush 尾音并处理残余分片 → join 线程。幂等。
    void stop();

    AsrEngine(const AsrEngine&) = delete;
    AsrEngine& operator=(const AsrEngine&) = delete;

private:
    void run(); ///< 工作线程主循环: 加载模型 → 消费 AudioChunk → flush+销毁(全程本线程)
    bool init_models(); ///< 在工作线程内创建 VAD + Recognizer, 结果经 init_result_ 回传
    void feed_vad(const float* samples, size_t n);
    void drain_vad_segments();           ///< 取出全部已完成分片并解码发布
    void publish_speech_start_if_rising(); ///< Detected() 上升沿 → SpeechStart
    std::string transcribe(const float* samples, int32_t n, double* latency_ms);

    AsrConfig cfg_;
    EventBus& bus_;
    EventBus::Queue* chunk_q_ = nullptr; ///< AudioChunk 订阅队列

    // sherpa-onnx 资源: 归 ASR 工作线程独占(创建/使用/销毁同线程, 见 asr_engine.cpp 头注)
    SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
    SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;

    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::promise<std::string> init_result_; ///< 模型加载结果回传给 start()
    bool speech_active_ = false; ///< Detected() 边沿检测状态
};

} // namespace sound_recv

#endif // SOUND_RECV_ASR_ENGINE_HPP
