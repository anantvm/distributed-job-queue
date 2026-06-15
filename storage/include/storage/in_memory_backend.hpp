#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// in_memory_backend.hpp
//
// Volatile in-memory implementation of IStorageBackend.
// Primary purpose: unit testing without touching the filesystem.
//
// Thread-safe via a single std::mutex — fine for test concurrency loads.
// NOT suitable for production use (data lost on process exit).
// ─────────────────────────────────────────────────────────────────────────────

#include <storage/i_storage_backend.hpp>

#include <mutex>
#include <unordered_map>

class InMemoryBackend final : public IStorageBackend {
public:
    InMemoryBackend() = default;

    [[nodiscard]] VoidResult persist_job(const Job& job) override;

    [[nodiscard]] VoidResult update_status(
        const std::string& job_id,
        JobStatus           status,
        const std::string&  error = "") override;

    [[nodiscard]] VoidResult increment_retry(const std::string& job_id) override;

    [[nodiscard]] Result<std::vector<Job>> load_recoverable_jobs() override;

    [[nodiscard]] Result<std::optional<Job>> get_job(const std::string& job_id) override;

    [[nodiscard]] Result<int64_t> total_job_count() override;

    // Phase 3 lease management
    [[nodiscard]] VoidResult set_lease(const std::string& job_id,
                                       int64_t expires_at_ms) override;
    [[nodiscard]] VoidResult clear_lease(const std::string& job_id) override;
    [[nodiscard]] Result<std::vector<Job>> get_expired_leases(int64_t now_ms) override;

    // ── Test helpers ──────────────────────────────────────────────────────────
    void clear();
    [[nodiscard]] size_t size() const;

private:
    mutable std::mutex                       mutex_;
    std::unordered_map<std::string, Job>     jobs_;
};
