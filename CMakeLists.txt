# sound_recv — ATK-DLRK3588 端侧语音交互框架
#
# 交叉编译(详见 README.md):
#   mkdir -p build/build_linux_aarch64
#   cd build/build_linux_aarch64
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../aarch64-toolchain.cmake ../..
#   make -j && make install
#
# 产物: install/sound_recv/bin/sound_recv + install/sound_recv/lib/*.so (RPATH=$ORIGIN/../lib)

cmake_minimum_required(VERSION 3.16)
project(sound_recv LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "仅在与板端匹配的 GCC 交叉工具链下验证过")
endif()

# 编译告警: 工业级基线
add_compile_options(-Wall -Wextra)

# ---------------- 第三方库(项目内预编译, 见 3rdparty_libs/) ----------------

# sherpa-onnx v1.13.8 C-API (VAD + SenseVoice, 纯 CPU/A76 推理)
set(SHERPA_ONNX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_libs/sherpa-onnx)
add_library(sherpa_onnx_capi SHARED IMPORTED)
set_target_properties(sherpa_onnx_capi PROPERTIES
    IMPORTED_LOCATION ${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-c-api.so
    INTERFACE_INCLUDE_DIRECTORIES ${SHERPA_ONNX_DIR}/include)

# RKLLM v1.2.3 运行时 (DeepSeek 1.5B, NPU 推理)
# 注意: librkllmrt.so 依赖 OpenMP, 必须显式一并链接(沿用 llm_test 参考),
# 否则链接期报 omp_get_thread_num/GOMP_* 未定义。
set(RKLLM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_libs/rkllm)
add_library(rkllm_runtime SHARED IMPORTED)
set_target_properties(rkllm_runtime PROPERTIES
    IMPORTED_LOCATION ${RKLLM_DIR}/lib/librkllmrt.so
    INTERFACE_INCLUDE_DIRECTORIES ${RKLLM_DIR}/include)
add_library(rkllm_gomp SHARED IMPORTED)
set_target_properties(rkllm_gomp PROPERTIES
    IMPORTED_LOCATION ${RKLLM_DIR}/lib/libgomp.so.1)

# ALSA (libasound, 来自工具链 sysroot)
find_library(ALSA_LIBRARY NAMES asound REQUIRED)

find_package(Threads REQUIRED)

# ---------------- 主程序 ----------------

add_executable(sound_recv
    src/main.cpp
    src/audio_capture.cpp
    src/asr_engine.cpp
    src/rkllm_engine.cpp
)

target_include_directories(sound_recv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(sound_recv PRIVATE
    Threads::Threads
    ${ALSA_LIBRARY}
    sherpa_onnx_capi
    rkllm_runtime
    rkllm_gomp
)

# RPATH: 安装后 bin/sound_recv 从同级 ../lib 找动态库, 板端免设 LD_LIBRARY_PATH
set_target_properties(sound_recv PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH_USE_LINK_PATH OFF)

# ---------------- 安装 ----------------
# 安装到工程内 install/sound_recv/ (非系统路径, 免 root, 便于整目录推板)
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/install/sound_recv)

install(TARGETS sound_recv RUNTIME DESTINATION bin)

# 运行时动态库随包部署(板端 rootfs 不一定带这些库)
install(DIRECTORY ${SHERPA_ONNX_DIR}/lib/  DESTINATION lib FILES_MATCHING PATTERN "*.so*")
install(DIRECTORY ${RKLLM_DIR}/lib/        DESTINATION lib FILES_MATCHING PATTERN "*.so*")

install(DIRECTORY models/ DESTINATION models) # silero_vad 已包含; ASR/LLM 模型由用户放置
