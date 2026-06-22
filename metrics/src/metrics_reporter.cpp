// ─────────────────────────────────────────────────────────────────────────────
// metrics_reporter.cpp
//
// MetricsReporter: periodic background metrics aggregation loop.
//
// Every interval_ms milliseconds the reporter loop:
//   1. Calls get_metrics_fn_() to retrieve a snapshot of JobManager::Metrics.
//   2. Updates gauges in MetricsRegistry for queue depth, workers, leases.
//   3. Computes throughput: (completed_now - completed_prev) / interval_seconds.
//   4. Calls purge_latency_fn_() if set.
//   5. Logs a one-line summary.
//
// stop() sets running_=false and joins the thread. The loop checks running_
// between sleeps to exit promptly.
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/metrics_reporter.hpp>
#include <metrics/metrics_registry.hpp>
#include <manager/job_manager.hpp>
#include <common/logger.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

MetricsReporter::MetricsReporter(int interval_ms)
    : interval_ms_(interval_ms)
{
    // Pre-register gauges so they appear in prometheus_text() even if 0.
    auto& reg = MetricsRegistry::instance();
    reg.gauge("job_queue_depth",   "Number of jobs currently waiting in the queue");
    reg.gauge("active_workers",    "Number of workers currently registered");
    reg.gauge("active_leases",     "Number of currently active job leases");
    reg.gauge("job_throughput_rps","Completed jobs per second (rolling interval)");
}

MetricsReporter::~MetricsReporter() {
    stop();
}

// ─── start() ─────────────────────────────────────────────────────────────────

void MetricsReporter::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // Already running.
    }
    reporter_thread_ = std::thread([this] { reporter_loop(); });
    Logger::info("MetricsReporter",
        "Started — interval " + std::to_string(interval_ms_) + " ms");
}

// ─── stop() ──────────────────────────────────────────────────────────────────

void MetricsReporter::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (reporter_thread_.joinable()) {
        reporter_thread_.join();
    }
}

// ─── set_metrics_fn() ────────────────────────────────────────────────────────

void MetricsReporter::set_metrics_fn(std::function<JobManagerMetrics()> fn) {
    get_metrics_fn_ = std::move(fn);
}

// ─── set_purge_fn() ──────────────────────────────────────────────────────────

void MetricsReporter::set_purge_fn(std::function<void()> fn) {
    purge_latency_fn_ = std::move(fn);
}

// ─── reporter_loop() ─────────────────────────────────────────────────────────

void MetricsReporter::reporter_loop() {
    auto& reg = MetricsRegistry::instance();

    Gauge& g_queue_depth  = reg.gauge("job_queue_depth");
    Gauge& g_workers      = reg.gauge("active_workers");
    Gauge& g_leases       = reg.gauge("active_leases");
    Gauge& g_throughput   = reg.gauge("job_throughput_rps");

    const double interval_seconds =
        static_cast<double>(interval_ms_) / 1000.0;

    while (running_.load(std::memory_order_relaxed)) {
        // Sleep in small increments so stop() wakes us promptly.
        const int   kSliceMsMax = 50;  // check running_ every 50ms
        int         slept_ms    = 0;

        while (running_.load(std::memory_order_relaxed) && slept_ms < interval_ms_) {
            const int slice = std::min(kSliceMsMax, interval_ms_ - slept_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept_ms += slice;
        }

        if (!running_.load(std::memory_order_relaxed)) break;

        // ── 1. Retrieve job manager snapshot ─────────────────────────────────
        JobManagerMetrics m;
        if (get_metrics_fn_) {
            // get_metrics_fn_ returns a JobManagerMetrics (mirror of JobManager::Metrics).
            m = get_metrics_fn_();
        }

        // ── 2. Update gauges ──────────────────────────────────────────────────
        g_queue_depth.set(static_cast<int64_t>(m.queue_length));
        g_workers.set(static_cast<int64_t>(m.active_workers));
        g_leases.set(static_cast<int64_t>(m.active_leases));

        // ── 3. Compute throughput ─────────────────────────────────────────────
        const uint64_t completed_now = m.jobs_completed;
        const double delta = static_cast<double>(
            completed_now >= prev_completed_ ? completed_now - prev_completed_ : 0);
        const double rps = delta / interval_seconds;
        // Store as integer RPS scaled ×100 in the gauge so we retain one decimal
        // of precision without using a float gauge type.
        // For simplicity: store as a rounded integer rps value × 100.
        // Consumers can divide by 100 for the real value, or we just round.
        g_throughput.set(static_cast<int64_t>(rps * 100.0));
        prev_completed_ = completed_now;

        // ── 4. Purge stale latency entries ────────────────────────────────────
        if (purge_latency_fn_) {
            purge_latency_fn_();
        }

        // ── 5. Log summary ────────────────────────────────────────────────────
        std::ostringstream oss;
        oss << "[Metrics] queue=" << m.queue_length
            << " workers=" << m.active_workers
            << " leases=" << m.active_leases
            << " completed=" << completed_now
            << std::fixed << std::setprecision(1)
            << " rps=" << rps;
        Logger::info("MetricsReporter", oss.str());
    }

    Logger::info("MetricsReporter", "Reporter loop exited");
}
