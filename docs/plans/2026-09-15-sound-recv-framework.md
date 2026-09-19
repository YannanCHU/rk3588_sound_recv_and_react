# sound_recv 端侧语音交互框架 实施计划

> **执行说明:** 本计划按任务逐项执行，每个任务以可验证的构建/测试通过为完成标志。
> **注意:** 本工作区不是 git 仓库（用户未要求 git init），故各任务以"验证"代替"提交"作为关卡。

**目标:** 在 ATK-DLRK3588 上实现 麦克风采集 → VAD 分片 → SenseVoice ASR → DeepSeek RKLLM NPU 推理 → 终端流式输出 的完整语音交互框架。

**架构:** 3 工作线程（采集/ASR/LLM）+ 无调度线程的轻量事件总线（生产者线程内同步 fan-out 到各订阅者有界队列）。数据全程 move 转移，零跨层阻塞调用。

**技术栈:** C++17 / CMake 3.16+ / ALSA(libasound) / sherpa-onnx v1.13.8 C-API(Silero VAD + SenseVoice int8) / RKLLM v1.2.3(librkllmrt.so + libgomp.so) / 交叉工具链 aarch64-buildroot-linux-gnu- GCC 13.4.0。

**规格来源:** `design.md`（事件契约 4.1、目录规划 3.4、验证方案 6.1）+ 已批准的会话设计修订（3 线程 + 无调度线程 EventBus）。

## 全局约束

- 必须交叉编译（工具链 `/opt/atk-dlrk3588-toolchain`），禁止原生编译主程序；单元测试例外（主机 x86_64 编译运行）。
- 音频格式固定 16kHz / S16_LE / Mono，浮点归一化 [-1,1]。
- 注释、文档一律中文。
- 事件契约沿用 design.md 4.1 的 `EVT_*` 语义，映射为 `EventType` 枚举。
- 队列语义：满则**丢最旧**（音频保护实时性；Prompt 队列同理，用户新问题优先）。
- sherpa-onnx 代码必须对照 `3rdparty_libs/sherpa-onnx/include/sherpa-onnx/c-api.h`（v1.13.8 已核实签名）。
- RKLLM 代码必须对照 `3rdparty_libs/rkllm/include/rkllm.h`（v1.2.3 已核实签名）与 `../llm_test/atk_deepseek_demo/main.cc`（已跑通参考）。
- DeepSeek Prompt 模板字节序列从 llm_test main.cc:13-14 复制（U+FF5C/U+2581），禁止手打。
- 部署布局：`install/sound_recv/bin/sound_recv` + `install/sound_recv/lib/*.so`，RPATH=`$ORIGIN/../lib`。

## 依赖布局（Task 1 已完成并验证）

```
3rdparty_libs/
├── sherpa-onnx/
│   ├── include/sherpa-onnx/c-api.h        # v1.13.8 源码标签提取, 4723 行
│   └── lib/                                # 官方 aarch64 预编译
│       ├── libsherpa-onnx-c-api.so         # 4.4M  NEEDED: libonnxruntime.so 等
│       ├── libsherpa-onnx-cxx-api.so       # (带上但不用)
│       └── libonnxruntime.so               # 34M
└── rkllm/
    ├── include/rkllm.h                     # v1.2.3 (自 llm_test 拷贝)
    └── lib/
        ├── librkllmrt.so                   # 6.0M  NEEDED: libgomp.so.1
        ├── libgomp.so / libgomp.so.1       # SONAME=libgomp.so.1, 两名并存保部署
models/vad/silero_vad.onnx                  # 629K 已就位
```

已验证: glibc 符号版本 sherpa-onnx ≤ 2.17、rkllm ≤ 2.29，均 < 板端 2.41；全部为 aarch64 ELF。

---

### Task 1: 第三方库落地与兼容性验证 ✅ 已完成

（侦察阶段执行完毕，结果如上。）

---

### Task 2: 基础设施三件套 types/log/blocking_queue（TDD）

**Files:**
- Create: `include/types.hpp` — 事件契约、音频块、配置结构体、PCM 转换
- Create: `include/log.hpp` — 分级日志（stderr，带 [LEVEL][tag] 前缀）
- Create: `include/blocking_queue.hpp` — 有界阻塞队列模板
- Create: `tests/CMakeLists.txt`、`tests/test_main.cpp`（极简断言框架）
- Create: `tests/test_infra.cpp` — 队列与 PCM 转换测试

**Interfaces (Produces):**
```cpp
enum class EventType : uint8_t { AudioChunk, SpeechStart, TextRecognized, LlmToken, LlmDone, Error };
struct AudioChunkData   { std::vector<float> samples; };
struct SpeechStartData  { double monotonic_ts; };
struct TextRecognizedData { std::string text; double latency_ms; };
struct LlmTokenData     { std::string token; uint32_t index; };
struct LlmTokenDoneData { std::string full_text; uint32_t total_tokens; };
struct ErrorData        { std::string message; };
struct Event { EventType type; double ts; std::variant<...六种...> data; };  // 构造辅助 make_event<T>()
template<typename T> T* event_data(Event&);   // 封装 std::get_if
std::vector<float> pcm_s16_to_float(const int16_t* p, size_t n);  // /32768.0
struct AudioCaptureConfig / AsrConfig / LlmConfig  // 各引擎参数, 默认值即规格值
```

```cpp
template <typename T> class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity);
    bool push(T item);              // 满则丢最旧; 返回 false 表示发生丢弃; 关闭后静默丢弃
    bool pop(T& out);               // 阻塞; 空且已关闭 → false
    bool pop_for(T& out, std::chrono::milliseconds); // 超时 → false
    void close();                   // 唤醒所有等待者
    size_t size() const; uint64_t dropped() const; bool closed() const;
};
```

**Step 1:** 先写 `tests/test_infra.cpp` 完整测试代码（roundtrip 顺序、有界丢最旧+dropped 计数、阻塞 pop 被唤醒、pop_for 超时、close 唤醒、move-only 类型通过队列、pcm_s16 边界值 -32768/0/32767）。
**Step 2:** `cmake -S tests -B build/host_tests && cmake --build build/host_tests` 确认编译失败（头文件不存在）。
**Step 3:** 实现三个头文件。
**Step 4:** `ctest --test-dir build/host_tests --output-on-failure` 全绿。
**验证:** 测试全过；主机 g++ -std=c++17 -Wall -Wextra 无警告。

---

### Task 3: 事件总线 EventBus（TDD）

**Files:**
- Create: `include/event_bus.hpp`
- Create: `tests/test_event_bus.cpp`（并入 host 测试工程）

**Interfaces (Produces):**
```cpp
class EventBus {
public:
    using Queue = BlockingQueue<Event>;
    Queue* subscribe(EventType type, size_t capacity = 16); // 队列归 bus 所有
    void publish(Event e);        // 依次 push 到该类型全部订阅队列(满则各自丢最旧)
    void close_all();             // 关停所有队列
};
```
实现: `std::vector<std::unique_ptr<Queue>>` 管所有权 + 每类型订阅表 `std::array<std::vector<Queue*>, N>`；publish 持 registry 锁（无竞争时 ~20ns）。

**Step 1:** 测试: 订阅-发布到达、未订阅类型不到达、同类型两订阅者都收到、经总线丢最旧、close_all 解除阻塞。
**Step 2:** 跑测试确认失败。
**Step 3:** 实现 event_bus.hpp。
**Step 4:** 测试全绿。

---

### Task 4: 交叉构建骨架（toolchain + CMakeLists + 依赖冒烟）

**Files:**
- Create: `aarch64-toolchain.cmake`
- Create: `CMakeLists.txt`
- Create: `src/main.cpp`（临时冒烟版: 包含三个供应商头文件并打印版本宏，验证头文件/链接/RPATH——Task 8 替换为正式实现）

**关键内容:**
- toolchain: SYSTEM=Linux/PROCESSOR=aarch64, 编译器取 `/opt/atk-dlrk3588-toolchain`，CMAKE_SYSROOT 指向其内建 sysroot（含 libasound），FIND_ROOT_PATH_MODE PROGRAM=NEVER / LIBRARY=INCLUDE=ONLY。工具链目录做成 CACHE 变量可覆盖。
- CMakeLists: imported target `sherpa_onnx::capi`、`rkllm::runtime`；`find_library(ALSA asound REQUIRED)`；`INSTALL_RPATH "$ORIGIN/../lib"`；install 规则拷贝三个 .so + libgomp.so.1 到 `install/sound_recv/lib/`。

**Step 1:** 写三个文件。
**Step 2:** `mkdir -p build/build_linux_aarch64 && cd build/build_linux_aarch64 && cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake ../.. && make -j8 && make install`。
**Step 3:** 验证产物: `file` 确认 aarch64；readelf -d 确认 NEEDED 含 libsherpa-onnx-c-api.so/librkllmrt.so/libasound.so.2、RUNPATH=$ORIGIN/../lib；install 目录 .so 齐全。
**验证:** 冒烟二进制交叉编译链接成功 = 三个依赖全部接通。

---

### Task 5: ALSA 采集模块

**Files:**
- Create: `include/audio_capture.hpp`、`src/audio_capture.cpp`

**Interfaces:**
```cpp
class AudioCapture {
public:
    AudioCapture(AudioCaptureConfig cfg, EventBus& bus);
    ~AudioCapture();                       // RAII 调 stop()
    bool start(std::string* err = nullptr);// 打开设备 + 拉起线程
    void stop();                           // 置停止位 → join → 关设备
    uint64_t xrun_count() const;
private:
    void run();
    bool open_pcm(std::string& err);
    bool recover(int err);                 // -EPIPE→prepare; -ESTRPIPE→循环 resume
};
```

**关键算法（run 循环）:**
```
读 int16 缓冲[period=512帧, 32ms]
  err = snd_pcm_readi()
  -EPIPE    → snd_pcm_prepare(), xrun_count++, 连续 10 次失败 → 发布 Error 并退出
  -ESTRPIPE → while((err=snd_pcm_resume())==-EAGAIN) sleep(1ms)
  n>0       → pcm_s16_to_float → publish(EventType::AudioChunk, move(samples))
  检查 stop 原子位 → 退出
```
hw_params: S16_LE/16000Hz(精确)/1ch/period 512/buffer 4096（8×period，256ms 抗抖）。打开失败时日志输出设备名+errno 解读。

**验证:** 交叉编译零警告通过（ALSA 行为需板端实测，README 记验收步骤）。

---

### Task 6: ASR 引擎模块（sherpa-onnx VAD + SenseVoice）

**Files:**
- Create: `include/asr_engine.hpp`、`src/asr_engine.cpp`

**Interfaces:**
```cpp
class AsrEngine {
public:
    AsrEngine(AsrConfig cfg, EventBus& bus);
    ~AsrEngine();
    bool start(std::string* err = nullptr);   // 建 VAD+Recognizer(失败=NULL 检查)
    void stop();                              // flush 尾音 → 处理完残余分片 → join
private:
    void run();                               // 消费 AudioChunk 队列
    std::string transcribe(const float* s, int32_t n, double* latency_ms);
};
```

**关键调用序列（对照 c-api.h v1.13.8 核实）:**
```
VAD:     SherpaOnnxVadModelConfig{silero_vad{model,threshold=0.5,min_silence=0.5,
         min_speech=0.25,max_speech=10,window=512}, sample_rate=16000, num_threads=1}
         → SherpaOnnxCreateVoiceActivityDetector(&cfg, 30.0f /*缓冲秒*/)
Recognizer: SherpaOnnxOfflineRecognizerConfig{feat_config{16000,80},
         model_config.sense_voice{model, language="auto", use_itn=1},
         model_config{tokens, num_threads=4, provider="cpu"}}
         → SherpaOnnxCreateOfflineRecognizer (NULL→失败)
run():   pop_for(chunk, 100ms) → AcceptWaveform(samples.data(), n)
         Detected() 上升沿 → publish SpeechStart
         while(!Empty()): seg=Front → transcribe(seg->samples, seg->n)
           空文本 → log warn 丢弃; 否则 publish TextRecognized{latency_ms}
           DestroySpeechSegment(seg); Pop()
stop():  Flush() 强制收尾分片 → 同上处理 → Destroy
```

**验证:** 交叉编译零警告；模型文件缺失时启动失败日志清晰（路径打印）。

---

### Task 7: RKLLM 引擎模块

**Files:**
- Create: `include/rkllm_engine.hpp`、`src/rkllm_engine.cpp`

**Interfaces:**
```cpp
class RkllmEngine {
public:
    RkllmEngine(LlmConfig cfg, EventBus& bus);
    ~RkllmEngine();
    bool start(std::string* err = nullptr);  // rkllm_init
    void stop();                             // 等 join → rkllm_destroy
private:
    void run();                              // 消费 TextRecognized 队列
    static void on_result_cb(RKLLMResult*, void* userdata, LLMCallState);  // 蹦床
    void on_result(RKLLMResult*, LLMCallState);
    LLMHandle handle_ = nullptr;
};
```

**关键点:**
- Prompt 模板字节: 从 `../llm_test/atk_deepseek_demo/main.cc:13-14` 用脚本逐字节拷入源文件（U+FF5C `｜`、U+2581 `▁`），源码中注明"勿手打"。
- rkllm_run 为同步阻塞: run() 内 pop_for(200ms) 检查停止位；推理期间生成完当前轮才退出（SIGINT 等待当前回答完成，README 说明此语义）。
- callback: NORMAL→`printf("%s")+fflush(stdout)` 并 publish LlmToken{index++}; FINISH→换行 + publish LlmTokenDone{full_text,total_tokens}; ERROR→publish Error。
- RKLLMParam 全字段对照 rkllm.h:60-80 填写（mirostat=0, is_async=false, img_*=nullptr, extend_param.base_domain_id=0）。

**验证:** 交叉编译零警告；与 llm_test main.cc 逐行 diff 评审 rkllm_* 调用一致性。

---

### Task 8: 主程序整合

**Files:**
- Modify: `src/main.cpp`（替换 Task 4 冒烟版）

**结构:**
```
main(argc, argv):
  parse_args (--device/--vad-model/--asr-model/--asr-tokens/--llm-model/--help)
  打印横幅与关键配置
  sigempty+SIGINT/SIGTERM → pthread_sigmask(SIG_BLOCK) 先于线程创建
  构造 EventBus → 各引擎 start()
  while(sigwait(...) 不命中) ;  收到信号 → log → 依序 stop(): capture→asr→llm
  汇报 xrun 计数/丢弃计数
  退出码 0
```
停止顺序理由: 先停采集（不再产生音频）→ ASR flush 尾音并出最后分片 → LLM 完成当前推理后 destroy。三段 join 均有日志。

**验证:** 交叉编译全量通过；install 产物完整；readelf 复查 RPATH。

---

### Task 9: 文档与收尾回归

**Files:**
- Create: `README.md`（一键交叉编译、板端 /userdata/ 部署命令、模型下载地址与放置路径、分阶段验收步骤）
- Modify: `design.md` 3.3 运行视图（4线程→3线程+无调度线程EventBus 修订注记）
- Modify: `AGENTS.md`（状态更新: 代码已落地、sherpa-onnx 已 vendored、构建命令）

**验证（全量回归）:**
1. `ctest --test-dir build/host_tests --output-on-failure` 全绿
2. 从零重建: `rm -rf build/build_linux_aarch64 && mkdir -p build/build_linux_aarch64 && cd build/build_linux_aarch64 && cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake ../.. && make -j8 && make install` 零错误零警告
3. `readelf -d install/sound_recv/bin/sound_recv | grep -E "NEEDED|RUNPATH"` 与预期清单一致
4. 板端运行指引（README）覆盖 design.md 6.1 三步验收

---

## 自查记录

- **规格覆盖:** 用户规格 8 项文件 → Task 2-8 全部落位；design.md 事件契约 → EventType 六值映射；XRUN 恢复 → Task 5；空结果丢弃 → Task 6；线程隔离 → 队列+独立线程；RPATH → Task 4。
- **类型一致性:** Event/variant 载荷在各 Task 引用统一以 Task 2 定义为准（LlmTokenDoneData 命名注意，Task 7 用同名）。
- **无占位符:** 所有 API 签名均经本地头文件核实，模型/库路径均为已验证的真实路径。
