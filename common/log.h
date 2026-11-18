#pragma once

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>

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

        char body[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(mutex_);

        const char* name = names[static_cast<int>(level)];
        const char* t = tag ? tag : "log";

        // 连续相同的消息只输出一次, 换消息或析构时补一行"共出现 N 次",
        // 避免 per-frame 错误/重复 IPC 命令把日志刷屏。
        std::string key = std::string(name) + '\x1f' + t + '\x1f' + body;
        if (has_last_ && key == last_key_) {
            ++repeat_;
            return;
        }
        flush_repeats_locked();

        emit_locked(level, t, body);
        last_key_  = std::move(key);
        last_level_ = level;
        has_last_  = true;
        repeat_    = 0;
    }

    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_repeats_locked();
            if (file_) std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 输出一行 (调用方持锁)
    void emit_locked(LogLevel level, const char* tag, const char* body) {
        char time[20]{};
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(time, sizeof(time), "%H:%M:%S", &tm);

        const char* name = names[static_cast<int>(level)];
        std::fprintf(stderr, "[%s] [%s] [%s] %s\n", time, name, tag, body);
        if (file_) {
            std::fprintf(file_, "[%s] [%s] [%s] %s\n", time, name, tag, body);
            std::fflush(file_);
        }
    }

    // 若有被合并的重复消息, 补一行计数 (调用方持锁)
    void flush_repeats_locked() {
        if (!has_last_ || repeat_ == 0) {
            repeat_ = 0;
            return;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "(上一条消息共出现 %u 次, 已合并)", repeat_ + 1);
        emit_locked(last_level_, "log", msg);
        repeat_ = 0;
    }

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::FILE* file_ = nullptr;

    // 连续重复消息合并状态
    std::string last_key_;
    LogLevel    last_level_ = LogLevel::Info;
    bool        has_last_ = false;
    unsigned    repeat_   = 0;

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
