#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// latency_tracker.hpp
//
// Tracks per-job lifecycle timestamps and feeds latency histograms.
//
// Lifecycle events:
//   on_submitted()  — records the time a job entered the queue.
//   on_dispatched() — records the time a job was sent to a worker.
//   on_completed()  — computes all three latencies and observes them.
//   on_failed()     — discards tracking state without observing.
//
// Three histograms are fed via MetricsRegistry::instance():
//   • job_submit_to_dispatch_ms   — time from submission to dispatch
//   • job_dispatch_to_complete_ms — time from dispatch to completion
//   • job_end_to_end_ms           — time from submission to completion
//
// Thread-safety: internally mutex-protected; all methods are safe to call
// concurrently from the server's connection-handler threads.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Forward declaration — avoid pulling in metrics_registry.hpp transitively.
class Histogram;

// ─── JobTimestamps ────────────────────────────────────────────────────────────

struct JobTimestamps {
    int64_t submitted_ms{0};   // Unix epoch ms when job was submitted
    int64_t dispatched_ms{0};  // Unix epoch ms when job was dispatched to a worker
    int64_t completed_ms{0};   // Unix epoch ms when job was completed (or failed)
};

// ─── LatencyTracker ──────────────────────────────────────────────────────────

class LatencyTracker {
public:
    // ── Singleton ─────────────────────────────────────────────────────────────
    static LatencyTracker& instance();

    // ── Instance Methods ──────────────────────────────────────────────────────
    // Retrieves histogram references from MetricsRegistry::instance().
    // Registers them with standard boundaries if not already present.
    LatencyTracker();
    ~LatencyTracker() = default;

    // Non-copyable.
    LatencyTracker(const LatencyTracker&)            = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;

    // Clears all pending entries (useful for testing).
    void reset();

    // ── Lifecycle events ──────────────────────────────────────────────────────

    // Called when a job is accepted into the queue.
    void on_submitted(const std::string& job_id, int64_t ts_ms);

    // Called when a job is dispatched to a worker.
    void on_dispatched(const std::string& job_id, int64_t ts_ms);

    // Called when a job is completed.  Computes and records all three latencies
    // if the required timestamps are valid (> 0).  Removes the entry.
    void on_completed(const std::string& job_id, int64_t ts_ms);

    // Called when a job fails definitively (DLQ or terminal failure).
    // Removes tracking state without feeding histograms.
    void on_failed(const std::string& job_id, int64_t ts_ms);

    // Remove all pending entries whose submitted_ms < cutoff_ms.
    // Prevents unbounded memory growth from jobs that never complete/fail.
    void purge_older_than(int64_t cutoff_ms);

private:
    std::mutex                                   mu_;
    std::unordered_map<std::string, JobTimestamps> pending_;

    // Histogram references are stable for process lifetime (owned by registry).
    Histogram& h_submit_to_dispatch_;
    Histogram& h_dispatch_to_complete_;
    Histogram& h_end_to_end_;
};
