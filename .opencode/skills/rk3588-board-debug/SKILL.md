---
name: rk3588-board-debug
description: ATK-DLRK3588 板端调试技能。SSH/SCP 密码连接、交叉编译调试版、远程 gdb 崩溃定位(gdbserver + 交叉gdb)、板上工具可用性(ASan/valgrind 不可用)、sound_recv 调试开关、隔离实验方法论。Use when deploying to or debugging anything on the RK3588 board: 板端部署、崩溃、段错误、堆错误(corrupted/invalid pointer)、性能验证、远程调试。
---

# RK3588 板端调试

本技能沉淀自 sound_recv 工程的实战调试(2026-09)，覆盖"主机↔板子"协作的全部环节。
配套脚本在本目录 `scripts/` 下。所有路径以工程根目录(sound_recv/)为基准。

## 环境常量（ATK 出厂默认，变更时改这里和 scripts/）

| 项 | 值 |
|---|---|
| 板 IP / 账户 | `root@192.168.1.100`，密码 `root`（出厂默认） |
| 部署目录 | `/userdata/sound_recv/{bin,lib,models}` |
| 交叉 gdb | `/opt/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu-gdb`（**在顶层 bin/，不在目标子目录**） |
| 板端 gdbserver | `/usr/bin/gdbserver`（rootfs 自带，v15.2，与主机 gdb 同版本） |
| NFS 模型目录 | `/mnt/nfs/llm_test/mdl/` |

## 第 0 步：确认连接脚本存在

`/tmp` 会被定期清理（吃过亏），用前检查 `scripts/run_ssh.exp`、`run_scp.exp` 是否已就位于工程内，
执行时直接引用工程内路径即可（无需拷到 /tmp）。

## 连接与传输

```bash
scripts/run_ssh.exp "free -m"                          # 执行远程命令
scripts/run_scp.exp build/xxx/sound_recv /userdata/sound_recv/bin/   # 推文件
```

**三个必踩的坑：**
1. **后台进程挂起 SSH 会话**：板上 `nohup xxx > log 2>&1 < /dev/null &` 三件套缺一不可，
   尤其 `< /dev/null`（stdin 不重定向 ssh 永不返回）。即便如此仍可能挂住——用 `timeout 8` 包住
   expect 调用，进程照样会启动。
2. **pkill 误杀**：`pkill -f sound_recv` 会连命令行里含该串的 gdbserver 一起杀。精确杀：
   `pkill gdbserver`、`pkill -x sound_recv_dbg`。
3. **长命令超时**：expect 默认 timeout 90s；跑长测试复制 run_ssh.exp 改 `set timeout 300`，
   或直接在远程命令里用 `(nohup ... &)` + 轮询日志的方式。

## 构建带符号的调试版

```bash
# RelWithDebInfo: -O2 保持真实时序(时序敏感的崩溃可复现) + -g 符号
mkdir -p build/build_linux_aarch64_dbg && cd build/build_linux_aarch64_dbg
cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j$(nproc)
```

**关键坑：构建树产物的 RPATH 指向主机路径**（CMake 自动加的 build-RPATH），板上运行必须：
`LD_LIBRARY_PATH=/userdata/sound_recv/lib ./bin/sound_recv_dbg ...`。
正式版(`make install` 后的)才有 `$ORIGIN/../lib`，免设置。

## 远程 gdb：崩溃定位的最终手段（三板斧失效时用）

一键脚本（推二进制 → 板上起 gdbserver → 主机连接 → 崩溃时自动打印回溯）：

```bash
# 在工程根目录执行
scripts/gdb_remote.sh build/build_linux_aarch64_dbg/sound_recv "--no-llm --run-seconds 30"
```

手动两步版（理解原理用）：

```bash
# ① 板端: 起 gdbserver, 程序停在入口等连接
scripts/run_ssh.exp "cd /userdata/sound_recv && LD_LIBRARY_PATH=lib nohup /usr/bin/gdbserver :1234 ./bin/sound_recv_dbg --no-llm > /tmp/gdbserver.log 2>&1 < /dev/null &"

# ② 主机: 交叉 gdb 连接, continue 到崩溃, 打印回溯
/opt/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu-gdb -batch \
  -ex "file build/build_linux_aarch64_dbg/sound_recv" \
  -ex "set sysroot /opt/atk-dlrk3588-toolchain/aarch64-buildroot-linux-gnu/sysroot" \
  -ex "set solib-search-path 3rdparty_libs/sherpa-onnx/lib:3rdparty_libs/rkllm/lib" \
  -ex "target remote 192.168.1.100:1234" \
  -ex "handle SIGINT nostop noprint pass" \
  -ex "handle SIGTERM nostop noprint pass" \
  -ex "continue" -ex "bt 30" -ex "thread apply all bt 12"
```

要点：`set sysroot` 让 gdb 找到 libc/libstdc++ 符号；`solib-search-path` 找三方库符号；
SIGINT/SIGTERM 设 pass 让程序自身的 sigwait 收到信号（否则 gdb 拦截导致关停路径走不到）。

## 板上工具可用性（先知道什么不能用，别走弯路）

| 工具 | 状态 | 原因 |
|---|---|---|
| ASan | ❌ 永远起不来 | 内核 `VA_BITS=39`（用户态 512GB）容不下 libasan 固定阴影区 `0x600000000000`（96TB），必报 `sanitizer_allocator_primary64.h CHECK failed`。降 `mmap_rnd_bits`/overcommit 均无效 |
| valgrind | ❌ 自身崩溃 | 对 onnxruntime 指令模拟出错（`VALGRIND INTERNAL ERROR`）。另注意：它下面 sigwait 收不到信号，会按默认动作杀进程——用 `--run-seconds` 免信号关停 |
| MALLOC_CHECK_=3 | ✅ | glibc 堆检查，双释放/坏指针即刻报警 |
| /proc/asound/card1/pcm0c/sub0/hw_params | ✅ | **ALSA 测谎仪**：捕获进行时查内核实际参数（声道/速率/格式），不信用户态协商结果 |
| dmesg | ⚠️ 有限 | 用户态 abort 常无记录，别指望 |

## sound_recv 调试开关（免语音/免信号的全套自动化通道）

```bash
./bin/sound_recv --no-llm                       # 只跑 采集+ASR(排除 NPU/LLM 变量)
./bin/sound_recv --run-seconds 30               # 30秒后自动优雅关停(与 Ctrl+C 同路径, valgrind/脚本可用)
./bin/sound_recv --test-prompt '你好'            # 只测 LLM: 注入文本提问, 生成完自动退出
./bin/sound_recv --llm-template deepseek        # 切提示模板(默认 qwen3)
```

组合示例：`--no-llm --run-seconds 30`（纯 ASR 自测）、`--test-prompt '问题' --llm-model /mnt/nfs/.../xx.rkllm`（新模型免语音验证）。

板上另有隔离工具：`/userdata/debug_kit/`（sherpa-onnx-vad / sherpa-onnx-offline /
vad-with-offline-asr + test_silero_vad.wav），用于官方工具单独压测组件。

## 方法论（比工具更重要）

1. **先隔离再深挖**：用官方工具在板上单独压测可疑组件（模型/库），一次排除一类嫌疑。
2. **差分实验只改一个变量**：同模型×不同运行时、同程序×有无某线程……矩阵化记录证据。
3. **崩溃点漂移 = 堆被越界写**：别追"发现者"（报错的 free），去找"纵火犯"（越界写的人）。
4. **内核 `copy_to_user` 绕过一切用户态检测器**（ASan/valgrind 都看不到驱动写穿用户缓冲），
   怀疑驱动/ALSA 时直接查 `/proc` 实际参数。
5. **加载日志即版本指纹**：RKLLM 看 `rkllm-runtime version` / `rkllm-toolkit version` 行，
   版本偏差的症状是"能加载、大体能跑、特定条件下退化"——最阴险的一类。
