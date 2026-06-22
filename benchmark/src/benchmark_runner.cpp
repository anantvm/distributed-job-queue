// ─────────────────────────────────────────────────────────────────────────────
// benchmark_runner.cpp
//
// BenchmarkRunner::run() lifecycle:
//
//   1.  Print profile info
//   2.  wait_for_server() — poll MetricsClient::is_reachable() every 500 ms
//   3.  Record baseline MetricsSnapshot
//   4.  Start LoadGenerator
//   5.  Every 1 s for duration_sec seconds:
//       a.  Poll MetricsSnapshot
//       b.  Compute delta rps, queue_depth, latency from snapshot
//       c.  Append TimeSeries point
//       d.  Print live status line
//   6.  Stop LoadGenerator
//   7.  wait_for_queue_drain() — wait until queue_depth == 0
//   8.  Record final MetricsSnapshot
//   9.  Build and return BenchmarkReport
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/benchmark_runner.hpp>

#include <benchmark/load_generator.hpp>
#include <benchmark/metrics_client.hpp>
#include <common/logger.hpp>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

int64_t now_ms_sys() {
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

/// Format a double to N decimal places as a string.
std::string fmt_double(double v, int decimals = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << v;
    return oss.str();
}

/// Try common metric name variants; return 0.0 if none found.
double get_metric(const MetricsSnapshot& snap,
                  std::initializer_list<const char*> names) {
    for (const char* n : names) {
        double v = snap.get(n);
        if (v != 0.0) return v;
    }
    return 0.0;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

BenchmarkRunner::BenchmarkRunner(WorkloadProfile profile,
                                 std::string     server_host,
                                 uint16_t        data_port,
                                 uint16_t        metrics_port)
    : profile_(std::move(profile))
    , host_(std::move(server_host))
    , data_port_(data_port)
    , metrics_port_(metrics_port) {}

// ─────────────────────────────────────────────────────────────────────────────
// wait_for_server
// ─────────────────────────────────────────────────────────────────────────────

bool BenchmarkRunner::wait_for_server(int timeout_sec) {
    MetricsClient mc(host_, metrics_port_);

    Logger::info("BenchmarkRunner",
                 "Waiting for server at " + host_ + ":" +
                 std::to_string(metrics_port_) + " ...");

    const auto deadline =
        steady_clock::now() + seconds(timeout_sec);

    while (steady_clock::now() < deadline) {
        if (mc.is_reachable()) {
            Logger::info("BenchmarkRunner", "Server is reachable ✓");
            return true;
        }
        std::this_thread::sleep_for(milliseconds(500));
    }

    Logger::error("BenchmarkRunner",
                  "Server did not become reachable within " +
                  std::to_string(timeout_sec) + "s");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// wait_for_queue_drain
// ─────────────────────────────────────────────────────────────────────────────

bool BenchmarkRunner::wait_for_queue_drain(int timeout_sec) {
    MetricsClient mc(host_, metrics_port_);

    Logger::info("BenchmarkRunner", "Waiting for queue to drain ...");

    const auto deadline =
        steady_clock::now() + seconds(timeout_sec);

    while (steady_clock::now() < deadline) {
        MetricsSnapshot snap = mc.poll();
        double depth = get_metric(snap,
            {"job_queue_depth", "queue_depth", "pending_jobs"});

        if (depth <= 0.0) {
            Logger::info("BenchmarkRunner", "Queue drained ✓");
            return true;
        }

        Logger::info("BenchmarkRunner",
                     "  queue depth: " + fmt_double(depth, 0));
        std::this_thread::sleep_for(seconds(1));
    }

    Logger::warn("BenchmarkRunner",
                 "Queue did not fully drain within " +
                 std::to_string(timeout_sec) + "s");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// run
// ─────────────────────────────────────────────────────────────────────────────

BenchmarkReport BenchmarkRunner::run() {
    // ── 1. Print profile ──────────────────────────────────────────────────────
    std::cout << "\n"
              << "╔══════════════════════════════════════════════╗\n"
              << "║         Benchmark Profile: "
              << std::left << std::setw(18) << profile_.name
              << " ║\n"
              << "╠══════════════════════════════════════════════╣\n"
              << "║  Duration        : " << std::setw(25) << std::right
              << (std::to_string(profile_.duration_sec) + "s") << " ║\n"
              << "║  Target RPS      : " << std::setw(25)
              << fmt_double(profile_.submit_rate_rps, 1) << " ║\n"
              << "║  Submit threads  : " << std::setw(25)
              << profile_.num_submit_threads << " ║\n"
              << "║  Payload bytes   : " << std::setw(25)
              << profile_.payload_size_bytes << " ║\n"
              << "║  Max retries     : " << std::setw(25)
              << profile_.max_retries << " ║\n"
              << "╚══════════════════════════════════════════════╝\n\n"
              << std::flush;

    // ── 2. Wait for server ────────────────────────────────────────────────────
    if (!wait_for_server(30)) {
        Logger::warn("BenchmarkRunner",
                     "Proceeding despite server not being reachable.");
    }

    // ── 3. Baseline snapshot ──────────────────────────────────────────────────
    MetricsClient mc(host_, metrics_port_);
    MetricsSnapshot baseline = mc.poll();

    // Extract baseline completed-jobs counter.
    double baseline_completed =
        get_metric(baseline, {"jobs_completed_total", "job_completed_total",
                               "jobs_completed", "completed_jobs_total"});
    double baseline_failed =
        get_metric(baseline, {"jobs_failed_total", "job_failed_total",
                               "jobs_failed", "failed_jobs_total"});

    // ── 4. Start load generator ───────────────────────────────────────────────
    LoadGenerator gen(profile_, host_, data_port_);
    const int64_t run_start_ms = now_ms_sys();
    gen.start();

    // ── 5. Per-second monitoring loop ─────────────────────────────────────────
    BenchmarkReport report;
    report.profile_name = profile_.name;
    report.series.reserve(static_cast<size_t>(profile_.duration_sec));

    MetricsSnapshot prev_snap = baseline;
    double prev_completed = baseline_completed;
    double queue_depth_sum = 0.0;
    int64_t queue_depth_max = 0;

    for (int t = 0; t < profile_.duration_sec; ++t) {
        std::this_thread::sleep_for(seconds(1));

        MetricsSnapshot snap = mc.poll();
        const int64_t ts_now = now_ms_sys();

        // Deltas since last second.
        double completed_now =
            get_metric(snap, {"jobs_completed_total", "job_completed_total",
                               "jobs_completed", "completed_jobs_total"});
        double delta_completed = std::max(0.0, completed_now - prev_completed);
        prev_completed = completed_now;

        double queue_depth =
            get_metric(snap, {"job_queue_depth", "queue_depth",
                               "pending_jobs", "jobs_pending"});

        double e2e_p99 =
            get_metric(snap, {"job_end_to_end_ms_p99",
                               "job_e2e_latency_ms_p99",
                               "e2e_latency_p99_ms",
                               "job_latency_p99"});

        // Accumulate queue stats.
        queue_depth_sum += queue_depth;
        if (static_cast<int64_t>(queue_depth) > queue_depth_max) {
            queue_depth_max = static_cast<int64_t>(queue_depth);
        }

        // Append time-series point.
        BenchmarkReport::TimeSeries ts_point;
        ts_point.ts_ms       = ts_now;
        ts_point.rps         = delta_completed;  // per second
        ts_point.queue_depth = static_cast<int64_t>(queue_depth);
        ts_point.e2e_p99_ms  = e2e_p99;
        report.series.push_back(ts_point);

        // Live status output.
        LoadStats lstat = gen.stats();
        std::printf("[t=%3ds]  submitted=%7llu  rps=%7.1f  queue=%6.0f  p99=%7.1fms\n",
                    t + 1,
                    static_cast<unsigned long long>(lstat.jobs_submitted),
                    delta_completed,
                    queue_depth,
                    e2e_p99);
        std::fflush(stdout);

        prev_snap = snap;
    }

    // ── 6. Stop load generator ────────────────────────────────────────────────
    gen.stop();
    const int64_t run_end_ms = now_ms_sys();

    // ── 7. Wait for queue to drain ────────────────────────────────────────────
    static_cast<void>(wait_for_queue_drain(60));

    // ── 8. Final snapshot ─────────────────────────────────────────────────────
    MetricsSnapshot final_snap = mc.poll();

    // ── 9. Build report ───────────────────────────────────────────────────────
    LoadStats lstat = gen.stats();

    double final_completed =
        get_metric(final_snap, {"jobs_completed_total", "job_completed_total",
                                 "jobs_completed", "completed_jobs_total"});
    double final_failed =
        get_metric(final_snap, {"jobs_failed_total", "job_failed_total",
                                 "jobs_failed", "failed_jobs_total"});

    const double elapsed_sec =
        static_cast<double>(run_end_ms - run_start_ms) / 1000.0;

    const uint64_t total_completed =
        static_cast<uint64_t>(std::max(0.0, final_completed - baseline_completed));
    const uint64_t total_failed =
        static_cast<uint64_t>(std::max(0.0, final_failed - baseline_failed));

    report.duration_actual_ms  = run_end_ms - run_start_ms;
    report.jobs_submitted      = lstat.jobs_submitted + lstat.jobs_rejected;
    report.jobs_completed      = total_completed;
    report.jobs_failed         = total_failed;
    report.throughput_rps      = elapsed_sec > 0
                                     ? static_cast<double>(total_completed) / elapsed_sec
                                     : 0.0;
    report.submit_rps          = elapsed_sec > 0
                                     ? static_cast<double>(report.jobs_submitted) / elapsed_sec
                                     : 0.0;

    const int n_series = static_cast<int>(report.series.size());
    report.queue_depth_avg = n_series > 0
                                 ? queue_depth_sum / static_cast<double>(n_series)
                                 : 0.0;
    report.queue_depth_max = queue_depth_max;

    // Use final snapshot for aggregated latency percentiles.
    report.e2e_latency_p50_ms =
        get_metric(final_snap, {"job_end_to_end_ms_p50",
                                 "job_e2e_latency_ms_p50",
                                 "e2e_latency_p50_ms",
                                 "job_latency_p50"});
    report.e2e_latency_p99_ms =
        get_metric(final_snap, {"job_end_to_end_ms_p99",
                                 "job_e2e_latency_ms_p99",
                                 "e2e_latency_p99_ms",
                                 "job_latency_p99"});
    report.dispatch_latency_p50_ms =
        get_metric(final_snap, {"job_dispatch_latency_ms_p50",
                                 "dispatch_latency_p50_ms",
                                 "dispatch_ms_p50"});
    report.dispatch_latency_p99_ms =
        get_metric(final_snap, {"job_dispatch_latency_ms_p99",
                                 "dispatch_latency_p99_ms",
                                 "dispatch_ms_p99"});

    return report;
}
