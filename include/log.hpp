// log.hpp — 轻量分级日志(stderr), 板端零依赖。
//
// 用法: SR_LOG_INFO("capture") << "opened " << device;
// 输出: [I][capture] opened hw:1,0
//
// 线程安全: 每条日志单次 fprintf, glibc 下行级原子性足够。
// 级别编译期裁剪: NDEBUG 下 TRACE/INFO 仍保留(嵌入式现场排障需要), 如需裁剪改此处。
#ifndef SOUND_RECV_LOG_HPP
#define SOUND_RECV_LOG_HPP

#include <cstdio>
#include <string>

namespace sound_recv {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// 极简流式日志对象: 析构时一次 fprintf 输出整行。
class LogLine {
public:
    LogLine(LogLevel level, const char* tag)
    {
        const char* lv = "I";
        switch (level) {
        case LogLevel::Debug: lv = "D"; break;
        case LogLevel::Info: lv = "I"; break;
        case LogLevel::Warn: lv = "W"; break;
        case LogLevel::Error: lv = "E"; break;
        }
        std::fprintf(stderr, "[%s][%s] ", lv, tag);
    }
    ~LogLine() { std::fputc('\n', stderr); }

    template <typename T>
    LogLine& operator<<(const T& v)
    {
        std::fprintf(stderr, "%s", to_cstr(v));
        return *this;
    }

    LogLine& operator<<(const char* s) { std::fputs(s, stderr); return *this; }
    LogLine& operator<<(char* s) { std::fputs(s, stderr); return *this; } // strsignal() 等返回 char*
    LogLine& operator<<(const std::string& s) { std::fputs(s.c_str(), stderr); return *this; }
    LogLine& operator<<(char c) { std::fputc(c, stderr); return *this; }
    LogLine& operator<<(int v) { std::fprintf(stderr, "%d", v); return *this; }
    LogLine& operator<<(unsigned v) { std::fprintf(stderr, "%u", v); return *this; }
    LogLine& operator<<(long v) { std::fprintf(stderr, "%ld", v); return *this; }
    LogLine& operator<<(unsigned long v) { std::fprintf(stderr, "%lu", v); return *this; }
    LogLine& operator<<(double v) { std::fprintf(stderr, "%.3f", v); return *this; }

private:
    // 兜底: 交给 operator<<(const char*) 处理不了的类型走这里会被静态断言拦下
    template <typename T>
    static const char* to_cstr(const T&)
    {
        static_assert(sizeof(T) == 0, "LogLine: unsupported type, add an overload");
        return "";
    }
};

} // namespace sound_recv

#define SR_LOG_DEBUG(tag) ::sound_recv::LogLine(::sound_recv::LogLevel::Debug, tag)
#define SR_LOG_INFO(tag) ::sound_recv::LogLine(::sound_recv::LogLevel::Info, tag)
#define SR_LOG_WARN(tag) ::sound_recv::LogLine(::sound_recv::LogLevel::Warn, tag)
#define SR_LOG_ERROR(tag) ::sound_recv::LogLine(::sound_recv::LogLevel::Error, tag)

#endif // SOUND_RECV_LOG_HPP
