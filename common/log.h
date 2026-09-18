#pragma once

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

namespace danmaku_overlay {

enum class LogLevel : unsigned char { Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) { level_ = level; }

    bool set_file(const char* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) { std::fclose(file_); file_ = nullptr; }
        if (path && *path) file_ = std::fopen(path, "a");
        return file_ != nullptr;
    }

    void write(LogLevel level, const char* tag, const char* fmt, ...) {
        if (level < level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        char time[20]{};
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(time, sizeof(time), "%H:%M:%S", &tm);

        const char* name = names[static_cast<int>(level)];
        std::fprintf(stderr, "[%s] [%s] [%s] ", time, name, tag ? tag : "log");
        if (file_) std::fprintf(file_, "[%s] [%s] [%s] ", time, name, tag ? tag : "log");

        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fputc('\n', stderr);

        if (file_) {
            va_start(args, fmt);
            std::vfprintf(file_, fmt, args);
            va_end(args);
            std::fputc('\n', file_);
            std::fflush(file_);
        }
    }

    ~Logger() {
        if (file_) std::fclose(file_);
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    inline static constexpr const char* names[] = {
        "INFO", "WARN", "ERROR", "FATAL"
    };
};

#define DANMAKU_LOG_INFO(tag, fmt, ...)  ::danmaku_overlay::Logger::instance().write(::danmaku_overlay::LogLevel::Info,  tag, fmt, ##__VA_ARGS__)
#define DANMAKU_LOG_WARN(tag, fmt, ...)  ::danmaku_overlay::Logger::instance().write(::danmaku_overlay::LogLevel::Warn, tag, fmt, ##__VA_ARGS__)
#define DANMAKU_LOG_ERROR(tag, fmt, ...) ::danmaku_overlay::Logger::instance().write(::danmaku_overlay::LogLevel::Error, tag, fmt, ##__VA_ARGS__)
#define DANMAKU_LOG_FATAL(tag, fmt, ...) ::danmaku_overlay::Logger::instance().write(::danmaku_overlay::LogLevel::Fatal, tag, fmt, ##__VA_ARGS__)
#define LOG_TAG "danmaku_overlay"

} // namespace danmaku_overlay
