// rkllm_engine.hpp — DeepSeek RKLLM NPU 流式推理引擎(librkllmrt v1.2.3)。
//
// 职责: 消费 TextRecognized 事件 → 手动拼接 DeepSeek 专有 Prompt 模板 →
// rkllm_run() 同步阻塞推理(NPU) → 回调逐 token 流式打印到 stdout 并发布
// LlmToken 事件(供未来 TTS 消费) → FINISH 后发布 LlmTokenDone。
//
// 关键约束(AGENTS.md 坑点 2/3):
//   - DeepSeek 模板必须手动拼接, 其中 ｜=U+FF5C(全角), ▁=U+2581,
//     字节序列复制自 llm_test 参考工程 main.cc:13-14, 严禁手打。
//   - rkllm_run() 为同步阻塞, token 经回调返回; 必须独立线程, 不得阻塞采集。
//
// 停止语义: stop() 等待当前一轮生成完成(最长 max_new_tokens×单token耗时)再销毁。
// Ctrl+C 不会中断进行中的回答 —— 中途销毁 handle 有 NPU 崩溃风险, 不做。
#ifndef SOUND_RECV_RKLLM_ENGINE_HPP
#define SOUND_RECV_RKLLM_ENGINE_HPP

#include "event_bus.hpp"
#include "types.hpp"

#include <rkllm.h> // LLMHandle/RKLLMResult/LLMCallState(经 CMake 私有包含路径)

#include <atomic>
#include <string>
#include <thread>

namespace sound_recv {

class RkllmEngine {
public:
    RkllmEngine(LlmConfig cfg, EventBus& bus);
    ~RkllmEngine(); // RAII: 自动 stop()

    /// 加载 .rkllm 模型到 NPU(耗时数秒)并拉起推理线程。
    /// 模型不存在 / 版本与驱动不匹配 → false + 人话诊断。
    bool start(std::string* err = nullptr);

    /// 置停止位 → 等当前推理完成 → join → rkllm_destroy 释放 NPU 资源。幂等。
    void stop();

    RkllmEngine(const RkllmEngine&) = delete;
    RkllmEngine& operator=(const RkllmEngine&) = delete;

private:
    void run(); ///< 推理线程主循环: 消费 TextRecognized → rkllm_run
    int on_result(RKLLMResult* result, LLMCallState state); ///< 回调分派, 返回 0=继续推理
    static int result_callback_trampoline(RKLLMResult* result, void* userdata,
                                          LLMCallState state);

    LlmConfig cfg_;
    EventBus& bus_;
    EventBus::Queue* prompt_q_ = nullptr; ///< TextRecognized 订阅队列

    LLMHandle llm_handle_ = nullptr;

    std::thread thread_;
    std::atomic<bool> stop_flag_{false};

    // 回调侧状态(run 线程独占, rkllm_run 期间回调同步发生于本线程)
    std::string current_output_;  ///< 本轮已生成全文
    uint32_t token_index_ = 0;    ///< 本轮 token 序号
};

} // namespace sound_recv

#endif // SOUND_RECV_RKLLM_ENGINE_HPP
