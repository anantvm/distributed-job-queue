#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// load_generator.hpp — Multi-threaded, rate-limited job submitter
//
// Each thread runs an independent token-bucket loop (sleep-based) so the
// aggregate submit rate tracks the WorkloadProfile::submit_rate_rps target.
// Thread-local JobClients avoid any shared-socket contention.
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/workload_profile.hpp>

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ─── LoadStats ────────────────────────────────────────────────────────────────

struct LoadStats {
    uint64_t jobs_submitted{0};  ///< Submissions that received an OK from server
    uint64_t jobs_rejected{0};   ///< Submissions where server returned an error
    uint64_t submit_errors{0};   ///< Transport / connection errors
    int64_t  elapsed_ms{0};      ///< Wall-clock ms since start() was called
    double   actual_rps{0.0};    ///< (submitted + rejected) / elapsed_sec
};

// ─── LoadGenerator ────────────────────────────────────────────────────────────

class LoadGenerator {
public:
    LoadGenerator(WorkloadProfile profile, std::string host, uint16_t port);

    /// Spawn submit threads and start the load.
    void start();

    /// Signal threads to stop and join them.
    void stop();

    /// Thread-safe snapshot of current cumulative stats.
    [[nodiscard]] LoadStats stats() const;

private:
    // ── Internal ──────────────────────────────────────────────────────────────

    /// Main loop executed by each submit thread.
    void submit_loop(int thread_id);

    /// Sample a job_type from the weighted distribution.
    [[nodiscard]] std::string pick_job_type(std::mt19937& rng) const;

    /// Sample a Priority from the weighted distribution.
    [[nodiscard]] Priority pick_priority(std::mt19937& rng) const;

    /// Build a random ASCII payload of profile_.payload_size_bytes length.
    [[nodiscard]] std::string make_payload(std::mt19937& rng) const;

    // ── State ─────────────────────────────────────────────────────────────────
    WorkloadProfile     profile_;
    std::string         host_;
    uint16_t            port_;

    std::atomic<bool>      running_{false};
    std::vector<std::thread> threads_;

    std::atomic<uint64_t>  submitted_{0};
    std::atomic<uint64_t>  rejected_{0};
    std::atomic<uint64_t>  errors_{0};

    int64_t start_time_ms_{0};  ///< Set once in start(); read-only afterwards
};
