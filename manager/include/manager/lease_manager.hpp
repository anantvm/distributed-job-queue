#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// lease_manager.hpp — Job Lease / Deadline Manager (Phase 3)
//
// A "lease" is a time-bounded claim a worker holds on a job. If the worker
// does not report COMPLETE_JOB or FAIL_JOB before the lease expires, the
// LeaseManager fires the on_expire callback so the server can requeue the job.
//
// Why leases are better than heartbeat-only failure detection:
//   • Heartbeats tell us the worker *process* is alive.
//   • Leases tell us the worker is making *progress on this specific job*.
//   • A worker can be alive but stuck in an infinite loop — leases catch this.
//   • A worker can crash between heartbeats — leases provide a bounded gap.
//
// Thread safety:
//   grant() / revoke() may be called from any thread.
//   The background checker thread calls on_expire under its own scheduling.
//   on_expire is called WITHOUT any LeaseManager lock held — the callback may
//   safely call back into LeaseManager (e.g., revoke() inside on_expire).
//
// Lease lifecycle:
//   grant(job_id, worker_id, 30s)  →  [timer running]
//   revoke(job_id)                 →  [cancelled — completed/failed normally]
//   …or lease expires…             →  on_expire(job_id, worker_id) fired
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class LeaseManager {
public:
    using ExpireCallback =
        std::function<void(const std::string& job_id,
                           const std::string& worker_id)>;

    // Default lease duration — 30 seconds is generous for any reasonable job.
    // In production this should be configurable per job_type.
    static constexpr int64_t kDefaultLeaseDurationMs = 30'000;

    // Checker wakes every second to scan for expired leases.
    static constexpr int64_t kCheckIntervalMs = 1'000;

    LeaseManager() = default;
    ~LeaseManager();

    LeaseManager(const LeaseManager&)            = delete;
    LeaseManager& operator=(const LeaseManager&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Start the background checker thread with the given expiry callback.
    // Must be called before grant().
    void start(ExpireCallback on_expire);

    // Stop the background thread. Blocks until it exits.
    // In-flight callbacks may still be running when stop() returns.
    void stop();

    // ── Lease operations ──────────────────────────────────────────────────────

    // Grant a lease on `job_id` to `worker_id` for `duration_ms` milliseconds.
    // If a lease already exists for this job_id, it is replaced (re-dispatch).
    void grant(const std::string& job_id,
               const std::string& worker_id,
               int64_t duration_ms = kDefaultLeaseDurationMs);

    // Revoke (cancel) the lease for `job_id`. Called when the job completes or
    // fails normally — prevents a spurious expiry callback.
    // Returns true if a lease was found and cancelled, false if not found.
    bool revoke(const std::string& job_id);

    // ── Diagnostics ───────────────────────────────────────────────────────────

    [[nodiscard]] int active_lease_count() const;

    struct LeaseInfo {
        std::string job_id;
        std::string worker_id;
        int64_t     expires_at_ms{0};
    };

    [[nodiscard]] std::vector<LeaseInfo> list_leases() const;

private:
    struct Lease {
        std::string job_id;
        std::string worker_id;
        int64_t     expires_at_ms{0};
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Lease> leases_;  // job_id → lease

    ExpireCallback     on_expire_;
    std::thread        checker_thread_;
    std::atomic<bool>  running_{false};
    std::condition_variable cv_;   // wakes checker early on stop()

    void checker_loop();
    static int64_t now_ms();
};
