#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// worker_registry.hpp — Thread-safe registry of all connected workers
//
// Phase 3 addition. Tracks every worker that has sent REGISTER_WORKER, records
// heartbeat timestamps, and maintains the set of jobs each worker is executing.
//
// Thread safety:
//   All public methods are safe to call from any thread.
//   Uses std::shared_mutex so reads (list_workers, detect_failed) never block
//   each other, while writes (register, heartbeat, assign) take exclusive locks.
//
// Worker lifecycle:
//   register_worker() → assign_job() ←→ release_job() → deregister_worker()
//                           ↑ record_heartbeat() (periodic)
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─── WorkerInfo ────────────────────────────────────────────────────────────────

struct WorkerInfo {
    std::string worker_id;
    std::string host;        // IP address (populated from accept())

    int64_t registered_at_ms{0};
    int64_t last_heartbeat_ms{0};

    // Set of job_ids currently dispatched to this worker.
    std::set<std::string> assigned_jobs;

    // Derived state — computed on read.
    [[nodiscard]] bool is_idle()  const noexcept { return assigned_jobs.empty(); }
    [[nodiscard]] int  job_count() const noexcept {
        return static_cast<int>(assigned_jobs.size());
    }
};

// ─── WorkerRegistry ──────────────────────────────────────────────────────────

class WorkerRegistry {
public:
    WorkerRegistry() = default;

    // ── Registration ─────────────────────────────────────────────────────────

    // Add a new worker. If a worker with the same id already exists (reconnect
    // after crash), the entry is replaced with fresh timestamps.
    void register_worker(const std::string& worker_id,
                         const std::string& host = "");

    // Remove a worker. Silently does nothing if not found.
    void deregister_worker(const std::string& worker_id);

    // ── Heartbeat ────────────────────────────────────────────────────────────

    // Update last_heartbeat_ms to now. No-op if worker not found.
    void record_heartbeat(const std::string& worker_id);

    // ── Job assignment ────────────────────────────────────────────────────────

    // Record that job_id was dispatched to this worker.
    void assign_job(const std::string& worker_id, const std::string& job_id);

    // Remove job_id from the worker's assigned set (completed or failed).
    void release_job(const std::string& worker_id, const std::string& job_id);

    // ── Queries ───────────────────────────────────────────────────────────────

    // Return worker ids whose last_heartbeat_ms is older than `timeout_ms`.
    // Only considers workers that are currently registered.
    [[nodiscard]] std::vector<std::string>
    detect_failed(int64_t timeout_ms) const;

    // Return all job_ids assigned to a worker. Empty if worker not found.
    [[nodiscard]] std::set<std::string>
    jobs_of_worker(const std::string& worker_id) const;

    // Snapshot of a single worker. Returns nullopt if not found.
    [[nodiscard]] std::optional<WorkerInfo>
    get_worker(const std::string& worker_id) const;

    // Snapshot of all workers (for monitoring / health endpoint).
    [[nodiscard]] std::vector<WorkerInfo> list_workers() const;

    // Total number of registered workers.
    [[nodiscard]] int size() const;

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, WorkerInfo> workers_;

    static int64_t now_ms();
};
