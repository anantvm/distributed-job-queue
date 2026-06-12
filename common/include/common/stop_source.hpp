#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// stop_source.hpp
//
// Polyfill for std::stop_source / std::stop_token (C++20) for compilers that
// ship C++20 language features but not the full library (e.g., Apple Clang 14).
//
// Design:
//   StopSource: holds a shared_ptr<atomic<bool>> "stop flag".
//   StopToken:  cheap copy; reads the shared flag.
//
// This is intentionally minimal — just enough for cooperative cancellation
// of worker threads and blocking queue pops.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <memory>

class StopToken {
public:
    explicit StopToken(std::shared_ptr<std::atomic<bool>> flag)
        : flag_(std::move(flag)) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class StopSource {
public:
    StopSource()
        : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void request_stop() noexcept {
        flag_->store(true, std::memory_order_release);
    }

    [[nodiscard]] StopToken get_token() const { return StopToken{flag_}; }

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};
