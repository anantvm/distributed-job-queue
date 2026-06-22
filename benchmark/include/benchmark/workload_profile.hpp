#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// workload_profile.hpp — Describes a benchmark scenario
//
// A WorkloadProfile captures every tunable parameter that drives the load
// generator: rate, thread count, payload size, duration, and the job-type /
// priority distributions (expressed as relative weights so they sum to any
// positive total and are normalised at sample time).
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>

#include <string>
#include <vector>

struct WorkloadProfile {
    // ── Identity ──────────────────────────────────────────────────────────────
    std::string name{"baseline"};

    // ── Duration & Rate ───────────────────────────────────────────────────────
    int    duration_sec{60};         ///< How long to sustain the load
    double submit_rate_rps{100.0};   ///< Target aggregate submit rate (req/s)

    // ── Concurrency ───────────────────────────────────────────────────────────
    int num_submit_threads{4};       ///< Threads doing submissions
    int num_workers{4};              ///< Expected worker pool size (informational)

    // ── Payload ───────────────────────────────────────────────────────────────
    int payload_size_bytes{64};      ///< Size of the random payload to generate
    int max_retries{3};              ///< max_retries passed to each submit()

    // ── Job-type distribution ─────────────────────────────────────────────────
    struct JobTypeMix {
        std::string job_type;
        double      weight{1.0};  ///< Relative (un-normalised) weight
    };
    std::vector<JobTypeMix> job_type_mix{{"benchmark_job", 1.0}};

    // ── Priority distribution ─────────────────────────────────────────────────
    struct PriorityMix {
        Priority priority;
        double   weight{1.0};
    };
    std::vector<PriorityMix> priority_mix{
        {Priority::LOW,    0.1},
        {Priority::NORMAL, 0.8},
        {Priority::HIGH,   0.1},
    };

    // ── Factory methods for predefined profiles ───────────────────────────────

    /// Gentle warm-up: low rate, small payloads.
    static WorkloadProfile baseline();

    /// High-rate steady load sustained over a longer window.
    static WorkloadProfile sustained_load();

    /// Ramp up to a very high rate to exercise back-pressure.
    static WorkloadProfile burst_spike();

    /// Flood with HIGH-priority jobs to stress priority ordering.
    static WorkloadProfile priority_storm();

    /// Large payloads to stress serialisation / network bandwidth.
    static WorkloadProfile large_payload();
};
