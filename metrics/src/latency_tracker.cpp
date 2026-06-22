// ─────────────────────────────────────────────────────────────────────────────
// latency_tracker.cpp
//
// Implements LatencyTracker: per-job timestamp bookkeeping and histogram feeds.
//
// The three histograms use the standard millisecond bucket boundaries:
//   {1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000}
//
// on_completed() is the only path that feeds histograms. It computes each
// delta only when both endpoints are non-zero (guard against partial state).
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/latency_tracker.hpp>
#include <metrics/histogram.hpp>
#include <metrics/metrics_registry.hpp>

#include <common/logger.hpp>

#include <string>

// ─── Default histogram boundaries (milliseconds) ─────────────────────────────

static const std::vector<double> kDefaultBoundaries = {
    1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000
};

// ─── Constructor ──────────────────────────────────────────────────────────────

LatencyTracker::LatencyTracker()
    : h_submit_to_dispatch_(
          MetricsRegistry::instance().histogram(
              "job_submit_to_dispatch_ms",
              "Time in milliseconds from job submission to dispatch to a worker",
              kDefaultBoundaries))
    , h_dispatch_to_complete_(
          MetricsRegistry::instance().histogram(
              "job_dispatch_to_complete_ms",
              "Time in milliseconds from job dispatch to completion",
              kDefaultBoundaries))
    , h_end_to_end_(
          MetricsRegistry::instance().histogram(
              "job_end_to_end_ms",
              "End-to-end time in milliseconds from submission to completion",
              kDefaultBoundaries))
{
    Logger::debug("LatencyTracker", "Initialised with three latency histograms");
}

// ─── on_submitted ─────────────────────────────────────────────────────────────

void LatencyTracker::on_submitted(const std::string& job_id, int64_t ts_ms) {
    std::lock_guard<std::mutex> lk{mu_};
    auto& ts = pending_[job_id];
    ts.submitted_ms = ts_ms;
}

// ─── on_dispatched ────────────────────────────────────────────────────────────

void LatencyTracker::on_dispatched(const std::string& job_id, int64_t ts_ms) {
    std::lock_guard<std::mutex> lk{mu_};
    auto it = pending_.find(job_id);
    if (it == pending_.end()) {
        // Job was submitted before tracker was running — create a partial entry.
        pending_[job_id].dispatched_ms = ts_ms;
        return;
    }
    it->second.dispatched_ms = ts_ms;
}

// ─── on_completed ────────────────────────────────────────────────────────────

void LatencyTracker::on_completed(const std::string& job_id, int64_t ts_ms) {
    JobTimestamps ts_copy;
    {
        std::lock_guard<std::mutex> lk{mu_};
        auto it = pending_.find(job_id);
        if (it == pending_.end()) {
            return;  // No tracking state; silently skip.
        }
        ts_copy = it->second;
        ts_copy.completed_ms = ts_ms;
        pending_.erase(it);
    }

    // Observe latencies only when timestamps are valid (> 0).
    if (ts_copy.dispatched_ms > 0 && ts_copy.submitted_ms > 0) {
        const double submit_to_dispatch =
            static_cast<double>(ts_copy.dispatched_ms - ts_copy.submitted_ms);
        if (submit_to_dispatch >= 0.0) {
            h_submit_to_dispatch_.observe(submit_to_dispatch);
        }
    }

    if (ts_copy.dispatched_ms > 0 && ts_copy.completed_ms > 0) {
        const double dispatch_to_complete =
            static_cast<double>(ts_copy.completed_ms - ts_copy.dispatched_ms);
        if (dispatch_to_complete >= 0.0) {
            h_dispatch_to_complete_.observe(dispatch_to_complete);
        }
    }

    if (ts_copy.submitted_ms > 0 && ts_copy.completed_ms > 0) {
        const double end_to_end =
            static_cast<double>(ts_copy.completed_ms - ts_copy.submitted_ms);
        if (end_to_end >= 0.0) {
            h_end_to_end_.observe(end_to_end);
        }
    }
}

// ─── on_failed ────────────────────────────────────────────────────────────────

void LatencyTracker::on_failed(const std::string& job_id, int64_t /*ts_ms*/) {
    std::lock_guard<std::mutex> lk{mu_};
    pending_.erase(job_id);
}

// ─── purge_older_than ────────────────────────────────────────────────────────

void LatencyTracker::purge_older_than(int64_t cutoff_ms) {
    std::lock_guard<std::mutex> lk{mu_};
    std::size_t removed = 0;
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (it->second.submitted_ms > 0 && it->second.submitted_ms < cutoff_ms) {
            it = pending_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        Logger::debug("LatencyTracker",
            "Purged " + std::to_string(removed) + " stale job timestamp entries");
    }
}

// ─── Singleton & Testing ──────────────────────────────────────────────────────

LatencyTracker& LatencyTracker::instance() {
    static LatencyTracker inst;
    return inst;
}

void LatencyTracker::reset() {
    std::lock_guard<std::mutex> lk{mu_};
    pending_.clear();
}
