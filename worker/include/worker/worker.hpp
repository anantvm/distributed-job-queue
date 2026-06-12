#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// worker.hpp — Job Execution Thread (Phase 1)
//
// Uses std::thread + StopSource (our polyfill) instead of std::jthread,
// for compatibility with Apple Clang 14 / libc++ which doesn't ship the
// C++20 jthread/stop_token library even with -std=c++20.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/stop_source.hpp>
#include <manager/job_manager.hpp>
#include <worker/handler_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

class Worker {
public:
    Worker(int id, JobManager& manager, const HandlerRegistry& registry);
    ~Worker();

    // Non-copyable
    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;

    // Spawn the worker thread. Must be called exactly once.
    void start();

    // Request stop and block until the thread exits.
    void stop();

    // ── Observers ────────────────────────────────────────────────────────────
    [[nodiscard]] int         id()        const noexcept { return id_; }
    [[nodiscard]] std::string worker_id() const noexcept { return worker_id_; }
    [[nodiscard]] bool        running()   const noexcept { return running_.load(); }

    struct Stats {
        uint64_t jobs_executed{0};
        uint64_t jobs_succeeded{0};
        uint64_t jobs_failed{0};
        std::chrono::milliseconds total_execution_time{0};
    };

    [[nodiscard]] Stats stats() const;

private:
    int                    id_;
    std::string            worker_id_;
    JobManager&            manager_;
    const HandlerRegistry& registry_;
    StopSource             stop_source_;
    std::thread            thread_;
    std::atomic<bool>      running_{false};

    std::atomic<uint64_t>  jobs_executed_{0};
    std::atomic<uint64_t>  jobs_succeeded_{0};
    std::atomic<uint64_t>  jobs_failed_{0};
    std::atomic<int64_t>   total_exec_ms_{0};

    void run();
    void execute_job(const Job& job);

    [[nodiscard]] std::string component_name() const;
};
