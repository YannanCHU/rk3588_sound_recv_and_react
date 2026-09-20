# sound_recv — RK3588 端侧语音助手

麦克风采集 (ALSA) → VAD 分片 (Silero) → 语音识别 (SenseVoice, CPU/A76) → 大模型推理 (Qwen3-VL-2B, RKLLM/NPU) → 终端流式输出。

适用于 ATK-DLRK3588 开发板 (aarch64, Linux 6.1 Buildroot, glibc 2.41)。

## 架构一览

```
采集线程 ──publish(AudioChunk)──┐
ASR 线程 ──publish(TextRecognized / SpeechStart)──┼──▶ EventBus(无调度线程, 同步 fan-out)
LLM 线程 ──publish(LlmToken / LlmDone)──┘               │ move 进各订阅者有界队列
                                                    ▼
                              ASR队列(64)   LLM队列(2)   (未来: TTS/GPIO 队列)
```

- 三个工作线程, 各自消费专属事件队列; 队列满丢最旧, 采集线程永不阻塞。
- 扩展方式: `bus.subscribe(EventType::LlmToken, capacity)` 即接入 TTS/UI/GPIO, 现有模块零改动。
- 模块明细见 `design.md` 与各头文件注释。

## 目录结构

```
include/    头文件 (types/log/blocking_queue/event_bus/三引擎)
src/        实现 (main + 三引擎)
tests/      主机端(x86)单元测试, 不交叉编译
3rdparty_libs/  预编译 aarch64 库: sherpa-onnx v1.13.8 / rkllm v1.2.3(+libgomp)
models/     模型目录 (vad 已含; asr/llm 需自行放置, 见下)
aarch64-toolchain.cmake  交叉工具链配置
```

## 一键交叉编译 (Ubuntu 开发机)

前置: 工具链已安装于 `/opt/atk-dlrk3588-toolchain` (SDK 的 `.run` 包), cmake ≥ 3.16。

```bash
mkdir -p build/build_linux_aarch64
cd build/build_linux_aarch64
cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake ../..
make -j$(nproc)
make install
```

产物: `install/sound_recv/`

```
install/sound_recv/
├── bin/sound_recv        # RPATH=$ORIGIN/../lib, 免设 LD_LIBRARY_PATH
├── lib/*.so              # sherpa-onnx + onnxruntime + rkllm + libgomp
└── models/vad/silero_vad.onnx
```

主机端单元测试 (纯 C++ 逻辑, 不需要工具链):

```bash
cmake -S tests -B build/host_tests && cmake --build build/host_tests
ctest --test-dir build/host_tests --output-on-failure
```

## 模型准备 (放入 models/, 均需自行下载)

| 模型 | 放置路径 | 下载 |
|---|---|---|
| SenseVoice int8 (ASR) | `models/asr/sensevoice/model.int8.onnx`<br>`models/asr/sensevoice/tokens.txt` | [sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2) 解压取上述两文件 (HF 镜像: [csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17)) |
| LLM (.rkllm) | `models/llm/qwen3_vl_2b.rkllm`<br>(默认模型, 配套 `--llm-template qwen3`) | 官方模型仓库(提取码 `rkllm`): <https://console.box.lenovo.com/l/l0tXb8> 的 **1.2.3 目录**取 `QWEN3-VL-2B-instruct-w8a8_rk3588.rkllm` 改名放入。板端实测 8.5 tok/s, 全管线内存余量 ~900MB |
| Silero VAD | `models/vad/silero_vad.onnx` | 已包含在仓库中 |

注意: `.rkllm` 必须由 rkllm-toolkit **v1.2.3** 转换 (与板端 librkllmrt v1.2.3 / rknpu 驱动 0.9.8 匹配)。**版本偏差的实测症状**: 旧版 toolkit 转的 DeepSeek 模型在 1.2.3 运行时下能加载、短生成正常, 但长生成会退化为疯狂输出 `[PADxxxxx]` 占位 token。加载日志行 `rkllm-toolkit version` 必须显示 1.2.3。换用 DeepSeek 模型需加 `--llm-template deepseek`。

## 板端部署与运行

```bash
# 推送到板 (二选一)
scp -r install/sound_recv root@<板IP>:/userdata/
adb push install/sound_recv /userdata/          # adb 传输目录需逐个文件

# 板端运行 (RPATH 已内置, 无需 LD_LIBRARY_PATH)
cd /userdata/sound_recv
./bin/sound_recv                        # 全默认参数
./bin/sound_recv --device hw:1,0        # 指定声卡
./bin/sound_recv --help                 # 全部选项
```

## 板端验收步骤 (对应 design.md 6.1)

1. **音频通路** (工程外独立验证):
   `arecord -D hw:1,0 -d 3 -f S16_LE -r 16000 test.wav && aplay test.wav` — 确认声音清晰。
2. **ASR 独立验收**: 运行主程序后观察 `[I][asr] 识别(...)` 日志: 对麦克风说一句话,
   停顿 0.5s 后应输出识别文本与解码耗时 (A76 上 SenseVoice int8 单句应在 200ms 量级)。
3. **LLM 独立验收**: 识别文本出现后, 终端应立刻开始流式打印 DeepSeek 回答
   (`robot` 输出逐 token 刷新)。
4. **全链路**: 说话 → 分片 → 识别 → 流式回答 → 自动回到监听态, 循环多轮。
5. **优雅退出**: Ctrl+C → 尾音 flush → 当前回答完成后退出, 无 NPU 报错。

## 运行语义要点

- **一句话判定**: 说话后静音 0.5s 即认为结束 (`--vad-model` 同级参数在 types.hpp 中调整)。
- **打断**: LLM 回答期间新识别的文本进入队列(容量 2), 当前回答完成后按最新问题继续;
  旧排队问题被新问题挤掉 (语音交互语义)。
- **Ctrl+C**: 等待当前回答完整输出后才退出 (NPU 句中销毁有崩溃风险, 有意为之)。
- **日志**: 全部走 stderr, 模型回答走 stdout — 可分离重定向。

## 已知边界

- `hw:1,0` 为 ATK-DLRK3588 板载 ES8388 声卡的默认假设, 以 `arecord -l` 实测为准。
- RKLLM 上下文为单轮问答 (DeepSeek 模板无历史拼接), 多轮对话是后续工作。
- SenseVoice 前导静音/超长句由 VAD 的 min_speech/max_speech_duration 控制。
