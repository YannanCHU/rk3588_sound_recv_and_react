// types.hpp — 全局数据契约: 事件、音频块、配置结构体、PCM 转换。
//
// 本文件必须保持"纯 C++17 无外部依赖", 因为主机端单元测试(tests/)也包含它。
// ALSA / sherpa-onnx / rkllm 的头文件只允许出现在各引擎的 .hpp/.cpp 中。
//
// 事件契约与 design.md 4.1 节对应:
//   EVT_AUDIO_CHUNK     → EventType::AudioChunk
//   EVT_SPEECH_START    → EventType::SpeechStart
//   EVT_TEXT_RECOGNIZED → EventType::TextRecognized
//   EVT_LLM_TOKEN       → EventType::LlmToken
//   EVT_LLM_DONE        → EventType::LlmDone
//   (EVT_SPEECH_END 属于 ASR 引擎内部状态, 其载荷即分片本身, 不上总线)
#ifndef SOUND_RECV_TYPES_HPP
#define SOUND_RECV_TYPES_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace sound_recv {

// ---------------- 事件契约 ----------------

/// 事件类型。新增类型时同步更新 design.md 4.1 与 EventBus 的订阅表容量。
enum class EventType : uint8_t {
    AudioChunk,     ///< 采集线程 → ASR: 一块 16k/mono/float PCM
    SpeechStart,    ///< ASR: 检测到说话开始(供未来 UI/指示灯消费)
    TextRecognized, ///< ASR → LLM: 一句完整识别文本
    LlmToken,       ///< LLM: 流式 token(供未来 TTS 消费; 终端打印在回调内直出)
    LlmDone,        ///< LLM: 一轮生成结束
    Error,          ///< 任意模块: 非致命错误通报(致命错误直接导致 start() 失败)
};

/// 音频块: 归一化 float PCM [-1,1], 16kHz 单声道。全程以 move 转移所有权。
struct AudioChunkData {
    std::vector<float> samples;
};

/// 说话开始。monotonic_ts 为采集样本流的单调时间戳(秒)。
struct SpeechStartData {
    double monotonic_ts;
};

/// 一句识别结果。latency_ms 为 SenseVoice 解码耗时(性能监控用)。
struct TextRecognizedData {
    std::string text;
    double latency_ms;
};

/// LLM 流式 token。index 为本轮 token 序号(从 0 起)。
struct LlmTokenData {
    std::string token;
    uint32_t index;
};

/// 一轮 LLM 生成结束。full_text 为本轮全部 token 拼接。
struct LlmTokenDoneData {
    std::string full_text;
    uint32_t total_tokens;
};

/// 非致命错误通报。
struct ErrorData {
    std::string message;
};

/// 统一事件信封。ts 为进程启动起的单调秒数(避免时钟跳变)。
struct Event {
    EventType type;
    double ts;
    std::variant<AudioChunkData, SpeechStartData, TextRecognizedData, LlmTokenData,
                 LlmTokenDoneData, ErrorData>
        data;
};

/// 构造事件的便捷工厂(自动填时间戳)。
inline double monotonic_ts()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 1e9;
}

template <typename Payload>
inline Event make_event(EventType type, Payload&& payload)
{
    return Event{type, monotonic_ts(), std::forward<Payload>(payload)};
}

/// 安全取载荷; 类型不匹配返回 nullptr(等价 std::get_if 的语义封装)。
template <typename Payload>
inline Payload* event_data(Event& e)
{
    return std::get_if<Payload>(&e.data);
}

template <typename Payload>
inline const Payload* event_data(const Event& e)
{
    return std::get_if<Payload>(&e.data);
}

// ---------------- PCM 转换 ----------------

/// int16 PCM → 归一化 float [-1,1]。采集线程内联使用, 无堆分配以外开销。
inline std::vector<float> pcm_s16_to_float(const int16_t* p, size_t n)
{
    std::vector<float> out;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(p[i]) / 32768.0f;
    }
    return out;
}

// ---------------- 各引擎配置 ----------------

/// ALSA 采集配置。默认值即设计规格, 运行时可用命令行覆盖。
struct AudioCaptureConfig {
    std::string device = "hw:1,0";    ///< ES8388 声卡; 板上实测前先用 arecord -l 确认
    uint32_t sample_rate = 16000;     ///< 固定, ASR/VAD 模型输入要求
    uint32_t channels = 1;            ///< 固定单声道
    uint32_t period_frames = 512;     ///< 32ms/次读取, 恰为 Silero VAD 窗长
    uint32_t buffer_periods = 8;      ///< 内核缓冲 8×period ≈ 256ms, 抗调度抖动
    uint32_t max_consecutive_errors = 10; ///< 连续读取失败上限, 超过则退出采集
};

/// VAD + ASR 配置(sherpa-onnx C-API)。
struct AsrConfig {
    std::string vad_model = "models/vad/silero_vad.onnx";
    float vad_threshold = 0.5f;             ///< 语音概率阈值, 噪声环境可调
    float min_silence_duration = 0.5f;      ///< 静音多久判定一句话结束(秒)
    float min_speech_duration = 0.25f;      ///< 短于此的片段丢弃(秒)
    float max_speech_duration = 10.0f;      ///< 超长强制切分(秒)
    int32_t vad_window = 512;               ///< Silero 16k 标准窗长
    int32_t vad_num_threads = 1;            ///< VAD 极轻量, 单线程足够

    std::string asr_model = "models/asr/sensevoice/model.int8.onnx";
    std::string asr_tokens = "models/asr/sensevoice/tokens.txt";
    std::string language = "auto";          ///< SenseVoice 语言提示: auto/zh/en/ja/ko/yue
    bool use_itn = true;                    ///< 逆文本正则("一百二十" → "120")
    int32_t asr_num_threads = 4;            ///< SenseVoice 解码线程数, 跑 A76 大核
};

/// RKLLM 推理配置(librkllmrt v1.2.3)。
struct LlmConfig {
    std::string model_path = "models/llm/deepseek_1.5b.rkllm";
    int32_t max_new_tokens = 512;
    int32_t max_context_len = 2048;
    int32_t top_k = 1;             ///< 贪心解码(语音助手要稳定, 不要发散)
    float top_p = 0.95f;
    float temperature = 0.8f;
    float repeat_penalty = 1.1f;
};

} // namespace sound_recv

#endif // SOUND_RECV_TYPES_HPP
