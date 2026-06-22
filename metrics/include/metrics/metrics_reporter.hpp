#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// metrics_reporter.hpp
//
// Background thread that periodically snapshots job manager metrics and
// updates gauges in the MetricsRegistry. Also computes throughput (RPS)
// and triggers latency tracker purges.
//
// Design:
//   • Uses std::function callbacks to decouple from JobManager and
//     LatencyTracker, avoiding circular includes.
//   • The reporter_loop() sleeps for interval_ms between iterations and
//     wakes immediately on stop().
//   • All gauge updates are done through MetricsRegistry::instance() to
//     keep them visible to the HTTP metrics server.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Forward declarations — avoid pulling in manager headers from this header.
// The actual JobManager::Metrics type is used in the .cpp via the callback.
namespace JobManagerMetricsFwd {
    struct Snapshot {
        std::size_t queue_length{0};
        uint64_t    jobs_submitted{0};
        uint64_t    jobs_completed{0};
        uint64_t    jobs_failed{0};
        uint64_t    jobs_retried{0};
        int         active_workers{0};
        int         active_leases{0};
    };
}

// The callback returns a lightweight struct populated from JobManager::Metrics.
// To avoid a circular dependency between metrics_lib and manager_lib in headers,
// we define our own mirror struct. The .cpp adapts via the lambda at call site.
using JobManagerMetrics = JobManagerMetricsFwd::Snapshot;

class MetricsReporter {
public:
    explicit MetricsReporter(int interval_ms = 5000);
    ~MetricsReporter();

    // Non-copyable.
    MetricsReporter(const MetricsReporter&)            = delete;
    MetricsReporter& operator=(const MetricsReporter&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Launches the background reporter thread.  No-op if already running.
    void start();

    // Signals the reporter thread to stop and joins it.  Blocks until done.
    void stop();

    // ── Callback setters ──────────────────────────────────────────────────────

    // Set the function called every interval to retrieve job manager metrics.
    // The lambda should capture a reference/pointer to the JobManager and
    // return a JobManagerMetrics by value.
    void set_metrics_fn(std::function<JobManagerMetrics()> fn);

    // Set the function called every interval to purge stale latency entries.
    // Typically: [&tracker]{ tracker.purge_older_than(cutoff); }
    void set_purge_fn(std::function<void()> fn);

private:
    // ── Reporter loop (runs on reporter_thread_) ──────────────────────────────
    void reporter_loop();

    std::thread              reporter_thread_;
    std::atomic<bool>        running_{false};
    int                      interval_ms_;

    std::function<JobManagerMetrics()> get_metrics_fn_;
    std::function<void()>              purge_latency_fn_;

    // Previous completed count for RPS calculation.
    uint64_t prev_completed_{0};
};
