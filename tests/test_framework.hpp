// 极简单元测试框架: 无外部依赖, 断言失败计数, 进程退出码反映结果。
// 仅用于主机端测试, 不进入目标板固件。
#ifndef SOUND_RECV_TEST_FRAMEWORK_HPP
#define SOUND_RECV_TEST_FRAMEWORK_HPP

#include <cstdio>

namespace testfw {

extern int g_failures;
extern int g_checks;

inline void report(bool ok, const char* expr, const char* file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [FAIL] %s:%d  %s\n", file, line, expr);
    }
}

} // namespace testfw

#define CHECK(expr) ::testfw::report((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::testfw::report(((a) == (b)), #a " == " #b, __FILE__, __LINE__)

#endif // SOUND_RECV_TEST_FRAMEWORK_HPP
