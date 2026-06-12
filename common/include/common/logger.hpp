#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// logger.hpp
//
// Minimal structured logger.  Header-only using C++17 inline statics so there
// is exactly one shared mutex and log-level across all translation units.
//
// Design:
//   • Thread-safe via a single global mutex around the write.
//   • Logs to stdout (INFO/DEBUG) and stderr (WARN/ERROR).
//   • Timestamps at millisecond resolution.
//   • A "component" label on every line enables grep-based filtering.
//
// Usage:
//   Logger::Info("JobManager", "Submitted job " + job_id);
//   Logger::Error("Worker-3", "Handler threw: " + msg);
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    // Global minimum log level (can be changed at runtime).
    static void set_level(LogLevel level) noexcept { min_level_ = level; }
    [[nodiscard]] static LogLevel level() noexcept { return min_level_; }

    // Primary log method.
    static void log(LogLevel level, const std::string& component, const std::string& msg) {
        if (level < min_level_) return;

        const std::string line =
            timestamp() + "  " + level_str(level) + "  [" + component + "]  " + msg;

        std::lock_guard<std::mutex> lk{mutex_};
        if (level >= LogLevel::WARN) {
            std::cerr << line << '\n';
        } else {
            std::cout << line << '\n';
        }
    }

    // Convenience helpers.
    static void debug(const std::string& c, const std::string& m) { log(LogLevel::DEBUG, c, m); }
    static void info (const std::string& c, const std::string& m) { log(LogLevel::INFO,  c, m); }
    static void warn (const std::string& c, const std::string& m) { log(LogLevel::WARN,  c, m); }
    static void error(const std::string& c, const std::string& m) { log(LogLevel::ERROR, c, m); }

private:
    inline static std::mutex   mutex_;
    inline static LogLevel     min_level_{LogLevel::INFO};

    // "HH:MM:SS.mmm"
    static std::string timestamp() {
        using namespace std::chrono;
        const auto now    = system_clock::now();
        const auto time_t = system_clock::to_time_t(now);
        const auto ms     = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        // std::localtime is not thread-safe on all platforms; for production
        // code this would be replaced with a safer OS-specific call.
        oss << std::put_time(std::localtime(&time_t), "%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    static const char* level_str(LogLevel l) noexcept {
        switch (l) {
            case LogLevel::DEBUG: return "\033[36mDEBUG\033[0m";
            case LogLevel::INFO:  return "\033[32mINFO \033[0m";
            case LogLevel::WARN:  return "\033[33mWARN \033[0m";
            case LogLevel::ERROR: return "\033[31mERROR\033[0m";
        }
        return "?????";
    }
};
