#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// job_manager.hpp — Central Job Coordinator (Phase 3)
//
// Phase 3 adds WorkerRegistry and LeaseManager to the JobManager.
// The Server now drives these via the expose accessor methods.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/result.hpp>
#include <common/stop_source.hpp>
#include <manager/lease_manager.hpp>
#include <manager/priority_queue.hpp>
#include <manager/worker_registry.hpp>
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
    // Phase 3: also recovers any jobs with expired leases (crash recovery).
    [[nodiscard]] VoidResult initialize();

    // Graceful shutdown: stops lease manager, unblocks all waiting workers.
    void shutdown();

    // ── Job submission ────────────────────────────────────────────────────────

    [[nodiscard]] Result<std::string> submit_job(
        const std::string& job_type,
        const std::string& payload      = "",
        Priority           priority     = Priority::NORMAL,
        int                max_retries  = 3);

    // ── Job dispatch ──────────────────────────────────────────────────────────

    // Non-blocking pull; returns nullopt if queue is empty.
    [[nodiscard]] std::optional<Job> try_pull_job();

    // Blocking pull; returns nullopt on shutdown.
    [[nodiscard]] std::optional<Job> wait_for_job(const StopToken& stop_tok);

    // ── Job outcome ───────────────────────────────────────────────────────────

    [[nodiscard]] VoidResult complete_job(const std::string& job_id);

    [[nodiscard]] VoidResult fail_job(
        const std::string& job_id,
        const std::string& error,
        int                current_retry_count,
        int                max_retries);

    // Requeue a job whose worker died or whose lease expired.
    // Does NOT increment retry_count. Clears the lease.
    [[nodiscard]] VoidResult requeue_job(const std::string& job_id);

    // ── Phase 3: Lease-aware dispatch ─────────────────────────────────────────

    // Grant a lease when a job is dispatched to a worker.
    // Called by Server after dispatching a JOB_DISPATCH message.
    void grant_lease(const std::string& job_id,
                     const std::string& worker_id,
                     int64_t duration_ms = LeaseManager::kDefaultLeaseDurationMs);

    // Revoke a lease when a job outcome is reported (complete/fail).
    void revoke_lease(const std::string& job_id);

    // ── Phase 3: Registry accessors ───────────────────────────────────────────

    WorkerRegistry& worker_registry() { return registry_; }

    // ── Metrics ───────────────────────────────────────────────────────────────

    struct Metrics {
        std::size_t queue_length;
        uint64_t    jobs_submitted;
        uint64_t    jobs_completed;
        uint64_t    jobs_failed;
        uint64_t    jobs_retried;
        int         active_workers;
        int         active_leases;
    };

    [[nodiscard]] Metrics get_metrics() const;

    // ── Storage access (for tests) ────────────────────────────────────────────
    [[nodiscard]] IStorageBackend& storage() { return *storage_; }

private:
    std::unique_ptr<IStorageBackend> storage_;
    ThreadSafePriorityQueue          queue_;
    WorkerRegistry                   registry_;
    LeaseManager                     lease_mgr_;

    std::atomic<uint64_t> jobs_submitted_{0};
    std::atomic<uint64_t> jobs_completed_{0};
    std::atomic<uint64_t> jobs_failed_{0};
    std::atomic<uint64_t> jobs_retried_{0};

    // Called by LeaseManager background thread when a lease expires.
    void on_lease_expired(const std::string& job_id,
                          const std::string& worker_id);

    [[nodiscard]] static int64_t now_ms();
};
