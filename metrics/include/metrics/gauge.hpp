#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gauge.hpp
//
// Lock-free signed gauge backed by std::atomic<int64_t>.
// Suitable for tracking current state: queue depth, active workers, etc.
//
// Unlike counters, gauges can go up and down freely.
// Non-copyable; must be registered once in MetricsRegistry and held by ref.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>

class Gauge {
public:
    Gauge() noexcept : value_(0) {}
    ~Gauge() = default;

    // Non-copyable, non-movable.
    Gauge(const Gauge&)            = delete;
    Gauge& operator=(const Gauge&) = delete;
    Gauge(Gauge&&)                 = delete;
    Gauge& operator=(Gauge&&)      = delete;

    // Set to an absolute value.
    void set(int64_t v) noexcept {
        value_.store(v, std::memory_order_relaxed);
    }

    // Increment by n (default 1).
    void increment(int64_t n = 1) noexcept {
        value_.fetch_add(n, std::memory_order_relaxed);
    }

    // Decrement by n (default 1).
    void decrement(int64_t n = 1) noexcept {
        value_.fetch_sub(n, std::memory_order_relaxed);
    }

    // Return current value.
    [[nodiscard]] int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> value_;
};
