#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// counter.hpp
//
// Lock-free monotonically-increasing counter backed by std::atomic<uint64_t>.
// Suitable for tracking total events (submitted jobs, completed jobs, etc.).
//
// Naming convention: metric names should end in _total per Prometheus style.
// Non-copyable; must be registered once in MetricsRegistry and held by ref.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>

class Counter {
public:
    Counter() noexcept : value_(0) {}
    ~Counter() = default;

    // Non-copyable, non-movable (held by unique_ptr in registry).
    Counter(const Counter&)            = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&)                 = delete;
    Counter& operator=(Counter&&)      = delete;

    // Increment by n (default 1). n must be positive.
    void increment(uint64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }

    // Return current value.
    [[nodiscard]] uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    // Reset to zero (for test isolation — not typical in production Prometheus).
    void reset() noexcept {
        value_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> value_;
};
