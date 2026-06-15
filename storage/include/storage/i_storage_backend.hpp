#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// i_storage_backend.hpp
//
// Pure abstract interface for all storage backends.
//
// Design goals:
//   • All public methods are [[nodiscard]] — callers must check for errors.
//   • The interface is backend-agnostic (SQLite today, PostgreSQL tomorrow).
//   • Each method maps 1-to-1 to a discrete job lifecycle event, making the
//     implementation of the WAL-style "persist before acknowledge" easy.
//
// Implementing a new backend only requires overriding these methods.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/result.hpp>
#include <optional>
#include <string>
#include <vector>

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // ── Writes ────────────────────────────────────────────────────────────────

    // INSERT a new job. Must be called *before* the job enters the in-memory
    // queue (write-ahead pattern: durable first, then in-memory).
    [[nodiscard]] virtual VoidResult persist_job(const Job& job) = 0;

    // UPDATE status + optional error message.
    [[nodiscard]] virtual VoidResult update_status(
        const std::string& job_id,
        JobStatus           status,
        const std::string&  error = "") = 0;

    // Atomically increment retry_count and reset status to PENDING.
    [[nodiscard]] virtual VoidResult increment_retry(const std::string& job_id) = 0;

    // ── Reads ─────────────────────────────────────────────────────────────────

    // Called on manager startup to rebuild the in-memory queue from durable
    // storage. Returns all PENDING and RUNNING jobs.
    [[nodiscard]] virtual Result<std::vector<Job>> load_recoverable_jobs() = 0;

    // Point lookup by job_id.
    [[nodiscard]] virtual Result<std::optional<Job>> get_job(
        const std::string& job_id) = 0;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    // Total number of jobs in the store (all statuses).
    [[nodiscard]] virtual Result<int64_t> total_job_count() = 0;

    // ── Phase 3: Lease management ─────────────────────────────────────────────

    // Atomically set lease_expires_at_ms for a RUNNING job.
    // Called immediately after dispatching a job to a worker.
    [[nodiscard]] virtual VoidResult set_lease(
        const std::string& job_id,
        int64_t            expires_at_ms) = 0;

    // Clear the lease (set to 0) for a job.
    // Called when the job completes, fails, or is requeued.
    [[nodiscard]] virtual VoidResult clear_lease(const std::string& job_id) = 0;

    // Return all jobs whose lease has expired (lease_expires_at_ms > 0 &&
    // lease_expires_at_ms <= now_ms). Used during crash recovery on startup.
    [[nodiscard]] virtual Result<std::vector<Job>> get_expired_leases(
        int64_t now_ms) = 0;
};
