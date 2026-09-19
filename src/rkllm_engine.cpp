// rkllm_engine.cpp — RKLLM 推理实现。头文件见 rkllm_engine.hpp。
//
// 全部 API 对照 3rdparty_libs/rkllm/include/rkllm.h (v1.2.3);
// 调用序列对照已跑通的 llm_test 参考工程 main.cc。
#include "rkllm_engine.hpp"

#include "log.hpp"

#include <rkllm.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sound_recv {

static constexpr const char* kTag = "llm";
static constexpr auto kPollTimeout = std::chrono::milliseconds(200);

// ---- DeepSeek Prompt 模板 ----
// 下方两行由脚本从 llm_test main.cc:13-14 逐字节拷贝生成, 严禁手打修改!
// 其中 ｜ 为全角 U+FF5C, ▁ 为 U+2581。
static const char* kPromptPrefix = "<｜begin▁of▁sentence｜><｜User｜>";
static const char* kPromptPostfix = "<｜Assistant｜>";

RkllmEngine::RkllmEngine(LlmConfig cfg, EventBus& bus)
    : cfg_(std::move(cfg))
    , bus_(bus)
{
}

RkllmEngine::~RkllmEngine()
{
    stop();
}

// 静态蹦床: C 回调签名 → 成员函数。
// v1.2.3 起回调返回 int: 0=继续推理, 1=暂停推理(我们不用暂停, 恒返 0)。
int RkllmEngine::result_callback_trampoline(RKLLMResult* result, void* userdata,
                                            LLMCallState state)
{
    return static_cast<RkllmEngine*>(userdata)->on_result(result, state);
}

int RkllmEngine::on_result(RKLLMResult* result, LLMCallState state)
{
    switch (state) {
    case RKLLM_RUN_NORMAL: {
        // 流式 token: 终端直出(用户规格) + 事件发布(未来 TTS/UI)
        const char* tok = result->text ? result->text : "";
        std::printf("%s", tok);
        std::fflush(stdout);
        current_output_ += tok;
        bus_.publish(make_event(EventType::LlmToken, LlmTokenData{tok, token_index_}));
        ++token_index_;
        break;
    }
    case RKLLM_RUN_WAITING:
        // 等待 UTF-8 多字节字符补全, 属正常内部状态, 忽略
        break;
    case RKLLM_RUN_FINISH:
        std::printf("\n");
        std::fflush(stdout);
        SR_LOG_INFO(kTag) << "生成结束: " << token_index_ << " tokens";
        bus_.publish(make_event(EventType::LlmDone,
                                LlmTokenDoneData{current_output_, token_index_}));
        break;
    case RKLLM_RUN_ERROR:
        SR_LOG_ERROR(kTag) << "推理发生错误";
        bus_.publish(make_event(EventType::Error, ErrorData{"RKLLM 推理错误"}));
        break;
    default:
        break;
    }
    return 0; // 继续推理
}

bool RkllmEngine::start(std::string* err)
{
    if (thread_.joinable()) {
        return true;
    }

    FILE* f = std::fopen(cfg_.model_path.c_str(), "rb");
    if (!f) {
        if (err) {
            *err = "RKLLM 模型不存在: '" + cfg_.model_path + "'";
        }
        SR_LOG_ERROR(kTag) << "模型不存在: " << cfg_.model_path;
        return false;
    }
    std::fclose(f);

    // 参数: 以运行时默认值为底(库版本演进时自动携带正确默认值), 再覆盖我们的配置。
    // 此写法对照官方 rkllm_api_demo v1.2.3 (llm_demo.cpp)。
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = cfg_.model_path.c_str();
    param.max_context_len = cfg_.max_context_len;
    param.max_new_tokens = cfg_.max_new_tokens;
    param.top_k = cfg_.top_k;
    param.top_p = cfg_.top_p;
    param.temperature = cfg_.temperature;
    param.repeat_penalty = cfg_.repeat_penalty;
    param.frequency_penalty = 0.0f;
    param.presence_penalty = 0.0f;
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 0; // NPU 域 0
    param.extend_param.embed_flash = 1;    // 词嵌入查表走 flash(官方 demo 同款)

    SR_LOG_INFO(kTag) << "加载模型到 NPU (首次耗时数秒): " << cfg_.model_path;
    const auto t0 = std::chrono::steady_clock::now();
    LLMHandle handle = nullptr;
    int rc = rkllm_init(&handle, &param, &result_callback_trampoline);
    if (rc != 0 || !handle) {
        if (err) {
            *err = "rkllm_init 失败(rc=" + std::to_string(rc)
                + "): 模型损坏 / 与板端 rknpu 驱动版本不匹配(v1.2.3 ↔ 0.9.8)";
        }
        SR_LOG_ERROR(kTag) << "rkllm_init 失败 rc=" << rc;
        return false;
    }
    llm_handle_ = handle;
    const double load_s = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    SR_LOG_INFO(kTag) << "模型加载完成 (" << load_s << "s)";

    prompt_q_ = bus_.subscribe(EventType::TextRecognized, 2 /*新问题优先, 旧的丢弃*/);
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
    return true;
}

void RkllmEngine::run()
{
    Event e;
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (!prompt_q_->pop_for(e, kPollTimeout)) {
            continue;
        }
        auto* prompt = event_data<TextRecognizedData>(e);
        if (!prompt || prompt->text.empty()) {
            continue;
        }

        // ★ 单轮语义: 每次提问前显式清空 KV 缓存。
        // 实测仅靠 infer.keep_history=0 不足以阻止多轮累积 —— 数轮问答后
        // prompt+回答的总 token 数超过 max_context_len, 位置错乱导致生成
        // 退化: 贪婪解码锁死在词表尾部的占位 token 上, 疯狂输出 [PADxxxxx]。
        // (官方 demo 亦提供手动 rkllm_clear_kv_cache; 我们无 system prompt, 传 0)
        int rc_clear = rkllm_clear_kv_cache(llm_handle_, 0, nullptr, nullptr);
        if (rc_clear != 0) {
            SR_LOG_WARN(kTag) << "清空 KV 缓存失败 rc=" << rc_clear << " (继续推理)";
        }

        // 手动拼接 DeepSeek 模板(坑点 2)
        std::string text = std::string(kPromptPrefix) + prompt->text + kPromptPostfix;

        RKLLMInput input;
        std::memset(&input, 0, sizeof(input));
        input.input_type = RKLLM_INPUT_PROMPT;
        input.prompt_input = text.c_str();

        RKLLMInferParam infer;
        std::memset(&infer, 0, sizeof(infer));
        infer.mode = RKLLM_INFER_GENERATE;

        current_output_.clear();
        token_index_ = 0;

        const auto t0 = std::chrono::steady_clock::now();
        int rc = rkllm_run(llm_handle_, &input, &infer, /*userdata=*/this);
        const double gen_s = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (rc != 0) {
            SR_LOG_ERROR(kTag) << "rkllm_run 失败 rc=" << rc;
            bus_.publish(make_event(EventType::Error,
                                    ErrorData{"rkllm_run 失败 rc=" + std::to_string(rc)}));
        } else {
            SR_LOG_INFO(kTag) << "本轮生成耗时 " << gen_s << "s";
        }
    }
}

void RkllmEngine::stop()
{
    if (!thread_.joinable()) {
        return;
    }
    stop_flag_.store(true, std::memory_order_relaxed);
    thread_.join(); // 若正处于 rkllm_run, 等当前回答完成(见头文件停止语义说明)

    if (llm_handle_) {
        rkllm_destroy(llm_handle_);
        llm_handle_ = nullptr;
        SR_LOG_INFO(kTag) << "NPU 资源已释放";
    }
}

} // namespace sound_recv
