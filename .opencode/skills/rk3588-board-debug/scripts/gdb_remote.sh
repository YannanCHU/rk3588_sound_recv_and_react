#!/bin/bash
# 远程 gdb 一键抓崩溃现场: 推调试二进制 → 板上起 gdbserver → 主机交叉 gdb 连接 → 崩溃时打印回溯
# 用法(在工程根目录执行):
#   scripts/gdb_remote.sh <本地带符号二进制> "<板端运行参数>"
# 示例:
#   scripts/gdb_remote.sh build/build_linux_aarch64_dbg/sound_recv "--no-llm --run-seconds 30"
#   scripts/gdb_remote.sh build/build_linux_aarch64_dbg/sound_recv "--test-prompt '你好'"
set -e
BIN_LOCAL="$1"; ARGS="${2:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
NAME="$(basename "$BIN_LOCAL")"
GDB=/opt/atk-dlrk3588-toolchain/bin/aarch64-buildroot-linux-gnu-gdb
SYSROOT=/opt/atk-dlrk3588-toolchain/aarch64-buildroot-linux-gnu/sysroot

[ -x "$BIN_LOCAL" ] || { echo "用法: $0 <本地带符号二进制> \"<板端运行参数>\""; exit 1; }

# ① 推送二进制
"$HERE/run_scp.exp" "$BIN_LOCAL" /userdata/sound_recv/bin/ | tail -1

# ② 板上清理残留并启动 gdbserver(构建树产物需 LD_LIBRARY_PATH, 见 SKILL.md)
"$HERE/run_ssh.exp" "pkill gdbserver 2>/dev/null; pkill -x $NAME 2>/dev/null; sleep 1; cd /userdata/sound_recv && rm -f /tmp/gdbserver.log && LD_LIBRARY_PATH=/userdata/sound_recv/lib nohup /usr/bin/gdbserver :1234 ./bin/$NAME $ARGS > /tmp/gdbserver.log 2>&1 < /dev/null & echo gdbserver-started" || true
sleep 3

# ③ 主机连接, 放跑到崩溃, 自动打印回溯(总限时 300s, 时序敏感问题可加大)
timeout 300 "$GDB" -batch \
  -ex "set confirm off" -ex "set pagination off" -ex "set height 0" \
  -ex "file $BIN_LOCAL" \
  -ex "set sysroot $SYSROOT" \
  -ex "set solib-search-path $PWD/3rdparty_libs/sherpa-onnx/lib:$PWD/3rdparty_libs/rkllm/lib" \
  -ex "target remote 192.168.1.100:1234" \
  -ex "handle SIGINT nostop noprint pass" \
  -ex "handle SIGTERM nostop noprint pass" \
  -ex "continue" \
  -ex "printf \"\n===== 崩溃回溯 =====\n\"" \
  -ex "bt 30" \
  -ex "printf \"\n===== 全部线程 =====\n\"" \
  -ex "thread apply all bt 12"

echo
echo "板端程序日志: /tmp/gdbserver.log (scripts/run_ssh.exp \"cat /tmp/gdbserver.log\")"
