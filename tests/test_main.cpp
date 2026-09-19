// 测试入口: 汇总各模块测试并输出结果。
#include "test_framework.hpp"

#include <cstdio>

namespace testfw {
int g_failures = 0;
int g_checks = 0;
} // namespace testfw

// 各测试文件提供的入口
void run_infra_tests();
void run_event_bus_tests();

int main()
{
    std::printf("== sound_recv 主机单元测试 ==\n");

    std::printf("[infra]    基础设施: types/blocking_queue/pcm 转换\n");
    run_infra_tests();

    std::printf("[bus]      事件总线: EventBus\n");
    run_event_bus_tests();

    std::printf("== 结果: %d 项检查, %d 项失败 ==\n", testfw::g_checks, testfw::g_failures);
    return testfw::g_failures == 0 ? 0 : 1;
}
