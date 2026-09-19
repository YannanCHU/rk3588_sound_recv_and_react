// asr_engine.cpp — VAD + SenseVoice 实现。头文件见 asr_engine.hpp。
//
// 全部 API 签名对照 3rdparty_libs/sherpa-onnx/include/sherpa-onnx/c-api.h (v1.13.8)。
//
// ★ 线程归属(2026-09-19 板端调试结论): 模型的 创建→使用→销毁 必须全部在
//   同一个工作线程内完成。此前"主线程创建、ASR 线程推理、主线程销毁"的布局
//   在 ATK-DLRK3588 上触发堆损坏(glibc: munmap_chunk invalid pointer, 于
//   SileroVadModel 析构 SessionState 释放权重时引爆), 官方单线程工具无此问题。
//   现改为 run() 内自建自毁, start() 经 promise 同步等待加载结果。
#include "asr_engine.hpp"

#include "log.hpp"

#include <sherpa-onnx/c-api.h>

#include <chrono>
#include <cstring>
#include <utility>

namespace sound_recv {

static constexpr const char* kTag = "asr";
static constexpr auto kPollTimeout = std::chrono::milliseconds(100);

AsrEngine::AsrEngine(AsrConfig cfg, EventBus& bus)
    : cfg_(std::move(cfg))
    , bus_(bus)
{
}

AsrEngine::~AsrEngine()
{
    stop();
}

static bool file_readable(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fclose(f);
    return true;
}

bool AsrEngine::start(std::string* err)
{
    if (thread_.joinable()) {
        return true;
    }

    // 模型文件预检: 快速失败给出可读错误(真正加载在工作线程内)
    if (!file_readable(cfg_.vad_model)) {
        if (err) {
            *err = "VAD 模型不存在: '" + cfg_.vad_model + "'";
        }
        SR_LOG_ERROR(kTag) << "VAD 模型不存在: " << cfg_.vad_model;
        return false;
    }
    if (!file_readable(cfg_.asr_model)) {
        if (err) {
            *err = "ASR 模型不存在: '" + cfg_.asr_model + "'";
        }
        SR_LOG_ERROR(kTag) << "ASR 模型不存在: " << cfg_.asr_model;
        return false;
    }
    if (!file_readable(cfg_.asr_tokens)) {
        if (err) {
            *err = "tokens 文件不存在: '" + cfg_.asr_tokens + "'";
        }
        SR_LOG_ERROR(kTag) << "tokens 文件不存在: " << cfg_.asr_tokens;
        return false;
    }

    chunk_q_ = bus_.subscribe(EventType::AudioChunk, 64 /* ≈2s 音频, ASR 解码期间缓冲 */);
    speech_active_ = false;
    stop_flag_.store(false, std::memory_order_relaxed);

    init_result_ = std::promise<std::string>(); // 每次启动使用新 promise
    auto fut = init_result_.get_future();
    thread_ = std::thread([this] { run(); });
    const std::string init_err = fut.get(); // 等待工作线程完成模型加载
    if (!init_err.empty()) {
        if (err) {
            *err = init_err;
        }
        thread_.join();
        return false;
    }
    SR_LOG_INFO(kTag) << "ASR 引擎已启动 (vad=" << cfg_.vad_model
                      << " asr=" << cfg_.asr_model << " threads=" << cfg_.asr_num_threads << ")";
    return true;
}

bool AsrEngine::init_models()
{
    // ---- VAD: Silero ----
    SherpaOnnxVadModelConfig vad_cfg;
    std::memset(&vad_cfg, 0, sizeof(vad_cfg));
    vad_cfg.silero_vad.model = cfg_.vad_model.c_str();
    vad_cfg.silero_vad.threshold = cfg_.vad_threshold;
    vad_cfg.silero_vad.min_silence_duration = cfg_.min_silence_duration;
    vad_cfg.silero_vad.min_speech_duration = cfg_.min_speech_duration;
    vad_cfg.silero_vad.max_speech_duration = cfg_.max_speech_duration;
    vad_cfg.silero_vad.window_size = cfg_.vad_window;
    vad_cfg.sample_rate = 16000;
    vad_cfg.num_threads = cfg_.vad_num_threads;
    vad_cfg.provider = "cpu";
    vad_cfg.debug = 0;

    // 内部环形缓冲 30 秒: 单次说话最长 10s(max_speech_duration 切分), 余量充足
    vad_ = const_cast<SherpaOnnxVoiceActivityDetector*>(
        SherpaOnnxCreateVoiceActivityDetector(&vad_cfg, 30.0f));
    if (!vad_) {
        init_result_.set_value("VAD 初始化失败(模型损坏或与 silero_vad 不匹配): "
                               + cfg_.vad_model);
        SR_LOG_ERROR(kTag) << "VAD 初始化失败";
        return false;
    }

    // ---- Recognizer: SenseVoice int8 ----
    SherpaOnnxOfflineRecognizerConfig asr_cfg;
    std::memset(&asr_cfg, 0, sizeof(asr_cfg));
    asr_cfg.feat_config.sample_rate = 16000;
    asr_cfg.feat_config.feature_dim = 80;
    asr_cfg.model_config.sense_voice.model = cfg_.asr_model.c_str();
    asr_cfg.model_config.sense_voice.language = cfg_.language.c_str();
    asr_cfg.model_config.sense_voice.use_itn = cfg_.use_itn ? 1 : 0;
    asr_cfg.model_config.tokens = cfg_.asr_tokens.c_str();
    asr_cfg.model_config.num_threads = cfg_.asr_num_threads;
    asr_cfg.model_config.provider = "cpu";
    asr_cfg.model_config.debug = 0;
    asr_cfg.decoding_method = "greedy_search";

    recognizer_ = const_cast<SherpaOnnxOfflineRecognizer*>(
        SherpaOnnxCreateOfflineRecognizer(&asr_cfg));
    if (!recognizer_) {
        SherpaOnnxDestroyVoiceActivityDetector(vad_);
        vad_ = nullptr;
        init_result_.set_value("SenseVoice 识别器初始化失败: " + cfg_.asr_model);
        SR_LOG_ERROR(kTag) << "识别器初始化失败";
        return false;
    }
    init_result_.set_value("");
    return true;
}

void AsrEngine::feed_vad(const float* samples, size_t n)
{
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, samples, static_cast<int32_t>(n));
}

void AsrEngine::publish_speech_start_if_rising()
{
    int32_t detected = SherpaOnnxVoiceActivityDetectorDetected(vad_);
    if (detected && !speech_active_) {
        bus_.publish(make_event(EventType::SpeechStart, SpeechStartData{monotonic_ts()}));
        SR_LOG_INFO(kTag) << "检测到说话开始";
    }
    speech_active_ = detected != 0;
}

void AsrEngine::drain_vad_segments()
{
    // Empty() == 1 表示无已完成分片
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
        const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(vad_);
        if (!seg) {
            break;
        }
        double latency_ms = 0.0;
        std::string text = transcribe(seg->samples, seg->n, &latency_ms);
        const int64_t dur_ms = static_cast<int64_t>(seg->n) * 1000 / 16000;
        SherpaOnnxDestroySpeechSegment(seg);
        SherpaOnnxVoiceActivityDetectorPop(vad_);

        if (text.empty()) {
            // design.md 6.2: 空结果不向 LLM 投递, 静默丢弃回到监听态
            SR_LOG_WARN(kTag) << "解码得到空文本, 丢弃 (" << dur_ms << "ms 分片)";
            continue;
        }
        SR_LOG_INFO(kTag) << "识别(" << dur_ms << "ms, 解码 " << latency_ms
                          << "ms): " << text;
        bus_.publish(make_event(EventType::TextRecognized,
                                TextRecognizedData{std::move(text), latency_ms}));
    }
}

std::string AsrEngine::transcribe(const float* samples, int32_t n, double* latency_ms)
{
    const auto t0 = std::chrono::steady_clock::now();

    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer_);
    if (!stream) {
        SR_LOG_ERROR(kTag) << "创建离解码流失败";
        return {};
    }
    SherpaOnnxAcceptWaveformOffline(stream, 16000, samples, n);
    SherpaOnnxDecodeOfflineStream(recognizer_, stream);

    const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
    std::string text = (result && result->text) ? result->text : "";
    if (result) {
        SherpaOnnxDestroyOfflineRecognizerResult(result);
    }
    SherpaOnnxDestroyOfflineStream(stream);

    *latency_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    return text;
}

void AsrEngine::run()
{
    // 模型 创建→使用→销毁 全程在本线程(线程自持有, 见文件头注释)
    if (!init_models()) {
        return;
    }

    Event e;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (chunk_q_->pop_for(e, kPollTimeout)) {
            if (auto* chunk = event_data<AudioChunkData>(e)) {
                feed_vad(chunk->samples.data(), chunk->samples.size());
                publish_speech_start_if_rising();
            }
        }
        // 无论是否新数据, 都检查已完成分片(静音超时的分片由时间驱动, 非数据驱动)
        drain_vad_segments();
    }

    // 收尾(本线程): 强制分割尾音, 不丢最后一句话, 然后销毁全部模型
    SherpaOnnxVoiceActivityDetectorFlush(vad_);
    drain_vad_segments();
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
    vad_ = nullptr;
    SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    recognizer_ = nullptr;
    SR_LOG_INFO(kTag) << "ASR 引擎已停止";
}

void AsrEngine::stop()
{
    if (!thread_.joinable()) {
        return;
    }
    stop_flag_.store(true, std::memory_order_relaxed);
    thread_.join(); // 模型已在 run() 末尾(ASR 线程内)完成销毁
}

} // namespace sound_recv
