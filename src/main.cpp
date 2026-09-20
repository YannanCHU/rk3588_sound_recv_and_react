// main.cpp — 进程入口: 参数解析 → 事件总线 → 三引擎编排 → sigwait 优雅退出。
//
// 启动顺序(慢者先行, 失败早退):
//   1. RkllmEngine  (模型载入 NPU, 耗时数秒, 不依赖音频)
//   2. AsrEngine    (VAD/ASR onnx 加载, 秒级)
//   3. AudioCapture (最后才开麦, 保证音频流一出即被消费)
// 停止顺序(逆向, 语义见各引擎头文件):
//   采集停 → ASR flush 尾音并出最后分片 → LLM 完成当前回答并释放 NPU
//
// 信号处理: sigwait 模式。SIGINT/SIGTERM 在任何线程创建前被阻塞,
// 由主线程 sigwait 同步收取 —— 无异步信号处理器, 无 async-signal-safe 约束。
#include "asr_engine.hpp"
#include "audio_capture.hpp"
#include "event_bus.hpp"
#include "log.hpp"
#include "rkllm_engine.hpp"
#include "types.hpp"

#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace sound_recv;

namespace {

// --run-seconds 看门狗置位: 不依赖信号(valgrind 等环境下 sigwait 收不到信号)
std::atomic<bool> g_auto_stop{false};

void print_usage(const char* prog)
{
    std::printf(
        "用法: %s [选项]\n"
        "端侧语音助手: 麦克风 → VAD → SenseVoice ASR → DeepSeek(RKLLM/NPU) → 流式打印\n"
        "\n"
        "选项:\n"
        "  --device <hw:X,Y>     ALSA 采集设备        (默认 hw:1,0)\n"
        "  --vad-model <path>    Silero VAD onnx      (默认 models/vad/silero_vad.onnx)\n"
        "  --asr-model <path>    SenseVoice int8 onnx (默认 models/asr/sensevoice/model.int8.onnx)\n"
        "  --asr-tokens <path>   tokens.txt           (默认 models/asr/sensevoice/tokens.txt)\n"
        "  --llm-model <path>    LLM .rkllm 模型路径   (默认 models/llm/deepseek_1.5b.rkllm)\n"
        "  --llm-template <t>    提示模板 qwen3|deepseek (默认 qwen3, 与模型配套)\n"
        "  --asr-threads <n>     ASR 解码线程数       (默认 4, A76 大核)\n"
        "  --no-llm              调试: 不加载 RKLLM, 只跑 采集+ASR\n"
        "  --run-seconds <n>     调试: n 秒后自动优雅关停\n"
        "  --test-prompt <text>  调试: 只测 LLM —— 注入文本提问, 生成完自动退出\n"
        "  -h, --help            本帮助\n",
        prog);
}

struct CliArgs {
    std::string device;
    std::string vad_model;
    std::string asr_model;
    std::string asr_tokens;
    std::string llm_model;
    std::string llm_template; // 空串 = 用默认(qwen3)
    int asr_threads = 0;  // 0 = 用默认值
    bool no_llm = false;  // 调试隔离开关: 不加载 RKLLM, 只跑 采集+ASR
    int run_seconds = 0;  // 调试: N 秒后自动走优雅关停(与 Ctrl+C 同路径)
    std::string test_prompt; // 调试: 只测 LLM —— 注入文本提问, 生成完自动退出
};

CliArgs parse_args(int argc, char** argv)
{
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                SR_LOG_ERROR("main") << "选项 " << name << " 缺少参数";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--device") {
            a.device = next("--device");
        } else if (arg == "--vad-model") {
            a.vad_model = next("--vad-model");
        } else if (arg == "--asr-model") {
            a.asr_model = next("--asr-model");
        } else if (arg == "--asr-tokens") {
            a.asr_tokens = next("--asr-tokens");
        } else if (arg == "--llm-model") {
            a.llm_model = next("--llm-model");
        } else if (arg == "--llm-template") {
            a.llm_template = next("--llm-template");
        } else if (arg == "--asr-threads") {
            a.asr_threads = std::atoi(next("--asr-threads"));
        } else if (arg == "--no-llm") {
            a.no_llm = true;
        } else if (arg == "--run-seconds") {
            a.run_seconds = std::atoi(next("--run-seconds"));
        } else if (arg == "--test-prompt") {
            a.test_prompt = next("--test-prompt");
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            SR_LOG_ERROR("main") << "未知选项: " << arg;
            print_usage(argv[0]);
            std::exit(2);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv)
{
    const CliArgs cli = parse_args(argc, argv);

    // 配置: 结构体默认值即规格值, CLI 仅覆盖显式给出的项
    AudioCaptureConfig cap_cfg;
    if (!cli.device.empty()) {
        cap_cfg.device = cli.device;
    }
    AsrConfig asr_cfg;
    if (!cli.vad_model.empty()) {
        asr_cfg.vad_model = cli.vad_model;
    }
    if (!cli.asr_model.empty()) {
        asr_cfg.asr_model = cli.asr_model;
    }
    if (!cli.asr_tokens.empty()) {
        asr_cfg.asr_tokens = cli.asr_tokens;
    }
    if (cli.asr_threads > 0) {
        asr_cfg.asr_num_threads = cli.asr_threads;
    }
    LlmConfig llm_cfg;
    if (!cli.llm_model.empty()) {
        llm_cfg.model_path = cli.llm_model;
    }
    if (!cli.llm_template.empty()) {
        llm_cfg.prompt_template = cli.llm_template;
    }

    std::printf("==== sound_recv 端侧语音助手 (RK3588) ====\n");
    std::fflush(stdout);

    // 信号阻塞必须先于任何线程创建(子线程继承掩码, 信号只进 sigwait)
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

    EventBus bus;
    AudioCapture capture(cap_cfg, bus);
    AsrEngine asr(asr_cfg, bus);
    RkllmEngine llm(llm_cfg, bus);

    std::string err;

    // ---- LLM 单元测试模式: 不开采集/ASR, 注入文本提问, 生成完自动退出 ----
    if (!cli.test_prompt.empty()) {
        if (cli.no_llm) {
            SR_LOG_ERROR("main") << "--test-prompt 与 --no-llm 互斥";
            return 2;
        }
        if (!llm.start(&err)) {
            SR_LOG_ERROR("main") << "LLM 引擎启动失败: " << err;
            return 1;
        }
        EventBus::Queue* done_q = bus.subscribe(EventType::LlmDone, 4);
        SR_LOG_INFO("main") << "测试模式: 注入提问 → " << cli.test_prompt;
        bus.publish(make_event(EventType::TextRecognized,
                               TextRecognizedData{cli.test_prompt, 0.0}));
        Event e;
        for (;;) {
            if (done_q->pop_for(e, std::chrono::milliseconds(200))) {
                break; // LlmDone: 生成结束
            }
            struct timespec ts = {0, 50 * 1000 * 1000};
            const int s = sigtimedwait(&sigset, nullptr, &ts);
            if (s == SIGINT || s == SIGTERM) {
                SR_LOG_INFO("main") << "收到信号, 中止生成";
                break;
            }
        }
        llm.stop();
        SR_LOG_INFO("main") << "测试模式结束";
        return 0;
    }

    // ---- 启动: 慢者先行, 任一失败即清理退出 ----
    if (!cli.no_llm) {
        if (!llm.start(&err)) {
            SR_LOG_ERROR("main") << "LLM 引擎启动失败: " << err;
            return 1;
        }
    }
    if (!asr.start(&err)) {
        SR_LOG_ERROR("main") << "ASR 引擎启动失败: " << err;
        llm.stop();
        return 1;
    }
    if (!capture.start(&err)) {
        SR_LOG_ERROR("main") << "音频采集启动失败: " << err;
        asr.stop();
        llm.stop();
        return 1;
    }

    SR_LOG_INFO("main") << "系统就绪: 设备 " << cap_cfg.device << " | ASR "
                        << asr_cfg.asr_model << " | LLM " << llm_cfg.model_path;
    std::printf("对麦克风说话即可, Ctrl+C 退出。\n\n");
    std::fflush(stdout);

    // 调试: 定时自动触发优雅关停(与 Ctrl+C 同一条路径)。
    // 直接置原子标志而非 raise(): valgrind 下 sigwait 收不到信号, 信号会被默认动作终止进程。
    if (cli.run_seconds > 0) {
        SR_LOG_INFO("main") << cli.run_seconds << " 秒后自动关停";
        std::thread([secs = cli.run_seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            g_auto_stop.store(true);
        }).detach();
    }

    // ---- 运行: 等待退出信号 或 自动关停标志 ----
    // sigtimedwait 轮询而非 sigwait 阻塞: 让 --run-seconds 的标志位有机会被检查
    int sig = 0;
    for (;;) {
        struct timespec ts = {0, 200 * 1000 * 1000}; // 200ms
        int s = sigtimedwait(&sigset, nullptr, &ts);
        if (s == SIGINT || s == SIGTERM) {
            sig = s;
            break;
        }
        if (g_auto_stop.load()) {
            break;
        }
    }
    if (sig != 0) {
        SR_LOG_INFO("main") << "收到信号 " << strsignal(sig) << ", 开始优雅关停…";
    } else {
        SR_LOG_INFO("main") << "到达自动关停时间, 开始优雅关停…";
    }

    // ---- 关停: 采集 → ASR → LLM(等当前回答完成) ----
    capture.stop();
    asr.stop();
    llm.stop();

    SR_LOG_INFO("main") << "采集统计: xrun=" << capture.xrun_count();
    SR_LOG_INFO("main") << "已全部退出";
    return 0;
}
