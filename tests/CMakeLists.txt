# 声明: 主机端(x86_64)单元测试工程, 独立于交叉编译的主工程。
# 构建: cmake -S tests -B build/host_tests && cmake --build build/host_tests
# 运行: ctest --test-dir build/host_tests --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(sound_recv_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(test_sound_recv
    test_main.cpp
    test_infra.cpp
    test_event_bus.cpp
)

# 仅测试纯头文件模块(不依赖 alsa/sherpa/rkllm 头, 它们只在交叉环境可用)
target_include_directories(test_sound_recv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_sound_recv PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_sound_recv PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME sound_recv_unit_tests COMMAND test_sound_recv)
