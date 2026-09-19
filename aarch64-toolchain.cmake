# aarch64-toolchain.cmake — RK3588 交叉工具链配置。
#
# 用法:
#   cmake -DCMAKE_TOOLCHAIN_FILE=<本文件> <源码目录>
#
# 工具链来源: atk-dlrk3588-toolchain-aarch64-buildroot-linux-gnu-x86_64_*.run
# (正点原子 ATK-DLRK3588 SDK 自带, 已安装于 /opt)。
# 该工具链与板端 Buildroot rootfs 同源: sysroot 内含 libasound.so 与 alsa 头文件,
# glibc 与板端(glibc 2.41)完全匹配, 无需额外 sysroot。
#
# 如需切换到其他工具链(如 Arm GNU aarch64-none-linux-gnu-), 覆盖缓存变量:
#   cmake -DSOUND_RECV_TOOLCHAIN_DIR=/path/to/toolchain -DCMAKE_TOOLCHAIN_FILE=...
#     -DSOUND_RECV_CROSS_PREFIX=aarch64-none-linux-gnu- ...
# 并按需设置 SOUND_RECV_SYSROOT。

# ---------------- 工具链选择(可缓存覆盖) ----------------
set(SOUND_RECV_TOOLCHAIN_DIR
    "/opt/atk-dlrk3588-toolchain"
    CACHE PATH "交叉工具链安装根目录")

set(SOUND_RECV_CROSS_PREFIX
    "aarch64-buildroot-linux-gnu-"
    CACHE STRING "工具链前缀(如 aarch64-none-linux-gnu-)")

# sysroot 为空时自动取工具链内建 sysroot; 外部 sysroot 走此变量覆盖
set(SOUND_RECV_SYSROOT
    ""
    CACHE PATH "外部 sysroot(留空则用工具链内建)")

# ---------------- 目标平台 ----------------
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---------------- 编译器 ----------------
find_program(CMAKE_C_COMPILER
    NAMES ${SOUND_RECV_CROSS_PREFIX}gcc
    PATHS ${SOUND_RECV_TOOLCHAIN_DIR}/bin
    NO_DEFAULT_PATH
    REQUIRED)
find_program(CMAKE_CXX_COMPILER
    NAMES ${SOUND_RECV_CROSS_PREFIX}g++
    PATHS ${SOUND_RECV_TOOLCHAIN_DIR}/bin
    NO_DEFAULT_PATH
    REQUIRED)

# ---------------- sysroot 与查找根 ----------------
if(SOUND_RECV_SYSROOT STREQUAL "")
    # 工具链内建 sysroot: ${TOOLCHAIN_DIR}/<去前缀目标名>/sysroot
    file(GLOB _tc_targets LIST_DIRECTORIES true "${SOUND_RECV_TOOLCHAIN_DIR}/*-linux-gnu*")
    foreach(_d ${_tc_targets})
        if(EXISTS "${_d}/sysroot" AND IS_DIRECTORY "${_d}/sysroot")
            set(SOUND_RECV_SYSROOT "${_d}/sysroot")
        endif()
    endforeach()
endif()

if(NOT SOUND_RECV_SYSROOT STREQUAL "")
    set(CMAKE_SYSROOT "${SOUND_RECV_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${SOUND_RECV_SYSROOT}")
endif()

# find_* 搜索策略: 库与头文件只在 sysroot 内找(保证目标板一致);
# 程序(如 ccache、第三方工具)允许在主机找。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
