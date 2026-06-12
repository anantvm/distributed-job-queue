#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// job_manager.hpp — Central Job Coordinator (Phase 1)
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/result.hpp>
#include <common/stop_source.hpp>
#include <manager/priority_queue.hpp>
#include <storage/i_storage_backend.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class JobManager {
public:
    explicit JobManager(std::unique_ptr<IStorageBackend> storage);
    ~JobManager();

    JobManager(const JobManager&)            = delete;
    JobManager& operator=(const JobManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Load recoverable jobs from storage into the in-memory queue.
    [[nodiscard]] VoidResult initialize();

    // Graceful shutdown: unblocks all waiting workers.
    void shutdown();

    // ── Job submission ────────────────────────────────────────────────────────

    // Persist → enqueue → return assigned job_id.
    [[nodiscard]] Result<std::string> submit_job(
        const std::string& job_type,
        const std::string& payload      = "",
        Priority           priority     = Priority::NORMAL,
        int                max_retries  = 3);

    // ── Job dispatch ──────────────────────────────────────────────────────────

    // Non-blocking pull; returns nullopt if queue is empty.
    [[nodiscard]] std::optional<Job> try_pull_job();

    // Blocking pull; returns nullopt on shutdown or stop_requested.
    [[nodiscard]] std::optional<Job> wait_for_job(const StopToken& stop_tok);

    // ── Job outcome ───────────────────────────────────────────────────────────

    [[nodiscard]] VoidResult complete_job(const std::string& job_id);

    [[nodiscard]] VoidResult fail_job(
        const std::string& job_id,
        const std::string& error,
        int                current_retry_count,
        int                max_retries);

    // ── Metrics ───────────────────────────────────────────────────────────────

    struct Metrics {
        std::size_t queue_length;
        uint64_t    jobs_submitted;
        uint64_t    jobs_completed;
        uint64_t    jobs_failed;
        uint64_t    jobs_retried;
    };

    [[nodiscard]] Metrics get_metrics() const;

    // ── Storage access (for tests) ────────────────────────────────────────────
    [[nodiscard]] IStorageBackend& storage() { return *storage_; }

private:
    std::unique_ptr<IStorageBackend> storage_;
    ThreadSafePriorityQueue          queue_;

    std::atomic<uint64_t> jobs_submitted_{0};
    std::atomic<uint64_t> jobs_completed_{0};
    std::atomic<uint64_t> jobs_failed_{0};
    std::atomic<uint64_t> jobs_retried_{0};

    [[nodiscard]] static int64_t now_ms();
};
