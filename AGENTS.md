# AGENTS.md — sound_recv 语音识别工程

基于 ATK-DLRK3588 开发板的端侧语音助手：麦克风采集(ALSA) → VAD(Silero) → ASR(sherpa-onnx SenseVoice) → LLM(RKLLM DeepSeek 1.5B, NPU) → 终端流式输出。

当前状态：**Phase 1 代码已落地**（3 工作线程 + 无调度线程 EventBus）。构建/测试/部署命令见 `README.md`，架构与事件契约以 `design.md` 为准（3.3 有实施修订注记），实施记录见 `docs/plans/2026-09-15-sound-recv-framework.md`。

## 常用命令

```bash
# 主机单元测试(纯 C++ 逻辑, x86 原生)
cmake -S tests -B build/host_tests && cmake --build build/host_tests && ctest --test-dir build/host_tests --output-on-failure

# 交叉编译主程序(out-of-source, 两级嵌套 build 目录)
mkdir -p build/build_linux_aarch64 && cd build/build_linux_aarch64
cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake ../.. && make -j$(nproc) && make install
# 产物 install/sound_recv/{bin,lib,models}  RPATH=$ORIGIN/../lib 板端免设库路径
```

## 环境

- 开发主机：WSL Ubuntu (x86_64)。**主程序必须交叉编译**（工具链 cmake 文件：`aarch64-toolchain.cmake`，缓存变量可换前缀/sysroot）；`tests/` 例外，主机原生编译。
- 目标板：ATK-DLRK3588 (aarch64, Linux 6.1 Buildroot, glibc 2.41, NPU 6 TOPS)。
- 交叉工具链：`/opt/atk-dlrk3588-toolchain`（GCC 13.4.0，前缀 `aarch64-buildroot-linux-gnu-`），sysroot 自带 libasound。
- 主机 cmake 3.28.3 可用。

## 工作区关键路径（相对本目录）

| 路径 | 用途 |
|---|---|
| `design.md` | 本工程设计文档（唯一事实来源） |
| `../llm_test/atk_deepseek_demo/` | RKLLM 推理参考工程：`main.cc`（RKLLM C API 用法）、`ARCHITECTURE.md`（深度分析，**但其声称的 v1.2.3 有误，见坑点 5**） |
| `../llm_test/atk_deepseek_demo/lib/` | ⚠️ 其 librkllmrt.so 实为 **v1.1.4**，勿拷贝使用 |
| `../../atk_dlrk3588_linux6.1/external/rknn-llm/` | RKLLM SDK v1.2.3 完整源：`rkllm-runtime/Linux/librkllm_api/aarch64/`、`doc/`（中英文 PDF API 文档）、`examples/rkllm_api_demo/`（官方示例）、`rknpu-driver/`（驱动 0.9.8） |

SDK 目录（`atk_dlrk3588_linux6.1/`）体积巨大，只读参考，不要修改或全量扫描，只访问上述具体路径。

## 构建说明

交叉编译通过 `-DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake` 传入（不再在 CMakeLists 硬编码编译器）。out-of-source 构建沿用 llm_test 模式：build 目录嵌套两级（`build/build_linux_aarch64`），cmake 参数是 `../..`。

第三方库已 vendored 到 `3rdparty_libs/`：
- `sherpa-onnx/` — v1.13.8 官方 aarch64 预编译（lib + include，glibc ≤2.17）
- `rkllm/` — v1.2.3（**取自 SDK `external/rknn-llm/rkllm-runtime/Linux/librkllm_api/`**，勿用 llm_test 拷贝版），含 rkllm.h + librkllmrt.so + libgomp.so/.so.1（SONAME=libgomp.so.1）

**链接 librkllmrt 必须同时显式链接 libgomp**（OpenMP 符号解析），CMake 中是 `rkllm_gomp` imported target。

## 板端部署与运行

- 传输：adb push 或 scp 到板子。
- 产物 RPATH=$ORIGIN/../lib，板端**无需** LD_LIBRARY_PATH（区别于 llm_test 旧模式）。
- NPU 驱动 rknpu.ko 已编入内核（0.9.8），设备节点 `/dev/dri/renderD128`。
- 音频格式约定：16kHz / S16_LE / Mono。通路验收用 `arecord -D hw:X,Y -d 3 -f S16_LE -r 16000 test.wav`。

## 关键坑点

1. **模型文件不在仓库中**：`*.rkllm` 与 SenseVoice 需另行获取（下载地址见 README.md "模型准备"）；silero_vad.onnx 已在 `models/vad/`。`.rkllm` 需在 PC 端用 rkllm-toolkit v1.2.3 从 HuggingFace 模型转换（SDK: `external/rknn-llm/rkllm-toolkit/packages/`）。
2. **DeepSeek Prompt 模板必须手动拼接**：`<｜begin▁of▁sentence｜><｜User｜>` + 输入 + `<｜Assistant｜>`。其中 `｜` 是全角字符 U+FF5C、`▁` 是 U+2581，直接从参考 demo 的 main.cc 复制，勿手打（本工程实现位于 `src/rkllm_engine.cpp`，勿改动该两行）。
3. **rkllm_run() 是同步阻塞调用**：token 通过 callback 流式返回（`RKLLM_RUN_NORMAL` 逐 token、`RKLLM_RUN_FINISH` 结束）。多线程设计中 LLM 推理必须在独立线程，不得阻塞音频采集线程。
4. **librkllmrt.so 依赖 libgomp.so**（OpenMP），部署和链接时两者都要带上。
5. **RKLLM 运行时版本必须与 .rkllm 模型及板端 rknpu 驱动匹配**（当前 runtime v1.2.3 / 驱动 0.9.8）。**版本不符的典型症状：rkllm_init 阶段直接段错误**。板端启动时看 banner 行 `rkllm-runtime version` 确认实际加载的版本。曾因 llm_test 的 librkllmrt.so 实为 v1.1.4 而踩坑（v1.1.4 回调为 void、v1.2.3 为 int 返回值，头文件不兼容，已修复）。
6. **sherpa-onnx 代码严格对照 vendored 的 c-api.h (v1.13.8)**：C API 返回 const 指针（`SherpaOnnxCreateOfflineStream` 等），句柄以 `const T*` 持有；改用其他版本需先 diff 头文件。
7. **ES8388 声卡只支持立体声（声道约束 2~2），mono 请求会被 set_channels 拒绝**。曾因忽略该返回码导致 hw_params 按驱动默认 2ch 运行、而缓冲按 mono 分配 → `snd_pcm_readi` 每 32ms 溢出 1KB 砸烂堆元数据（症状：`free(): corrupted unsorted chunks` / `munmap_chunk(): invalid pointer`，且崩溃点漂移于 VAD 销毁/识别器销毁/进程退出之间）。教训：**ALSA 每个 set_* 返回码必须检查**；声道协商失败则接受设备能力并软件下混（已实现于 `audio_capture.cpp`，检测方法：捕获进行时 `cat /proc/asound/card1/pcm0c/sub0/hw_params` 看内核实际参数）。
8. **板上调试手段**：ASan 不可用（内核 `VA_BITS=39` 容不下 libasan 固定阴影区 0x600000000000）；valgrind 会自身崩溃（对 onnxruntime 指令模拟出错）。可靠方案是**远程 gdb**：主机 `/opt/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu-gdb`（注意在顶层 bin/，不在目标目录）+ 板端自带 `/usr/bin/gdbserver`。另注意：valgrind 下 sigwait 收不到信号（会走默认动作杀进程），程序已用 `sigtimedwait` 轮询 + `--run-seconds N` 免信号自关停。
9. **onnxruntime 会话生命周期尽量单线程自持有**（已实现：ASR 线程内自建自毁 `AsrEngine::run()`）。虽最终根因是坑点 7 而非跨线程，但此布局消除了一个变量且架构更清晰。
10. **RKLLM 多轮问答必须每次提问前显式 `rkllm_clear_kv_cache`**：仅设 `infer.keep_history=0` 不足以阻止 KV 缓存跨轮累积，数轮后总 token 超过 `max_context_len` → 位置错乱 → 生成退化为**疯狂输出 `[PADxxxxx]` 占位 token**（.rkllm 转换时词表补齐到 151,936 条的尾部未用槽位）。已实现于 `rkllm_engine.cpp`（无 system prompt，keep_system_prompt 传 0）。

## 语言与规范

- 代码：C++17 + CMake 3.16+（现代 CMake，imported target 管理三方库，禁止硬编码编译器）。
- 注释、文档、提交说明用中文。
- 实际目录：`include/ src/ tests/ 3rdparty_libs/ models/ docs/`（design.md 3.4 的 `third_party/`、`configs/` 尚未需要，YAGNI；新增文件按现有归位）。
