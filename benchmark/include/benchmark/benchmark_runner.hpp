#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// benchmark_runner.hpp — Orchestrates one full benchmark run
//
// BenchmarkRunner ties together the LoadGenerator (submit side) and
// MetricsClient (observe side), drives the timing loop, accumulates the
// per-second TimeSeries, and produces a populated BenchmarkReport.
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/benchmark_report.hpp>
#include <benchmark/workload_profile.hpp>

#include <cstdint>
#include <string>

class BenchmarkRunner {
public:
    BenchmarkRunner(WorkloadProfile profile,
                    std::string     server_host,
                    uint16_t        data_port,
                    uint16_t        metrics_port);

    /// Execute the full benchmark lifecycle and return the aggregated report.
    [[nodiscard]] BenchmarkReport run();

private:
    /// Block until the metrics endpoint responds (or timeout_sec elapses).
    [[nodiscard]] bool wait_for_server(int timeout_sec = 30);

    /// Block until the server's queue_depth gauge reaches zero (jobs drained).
    [[nodiscard]] bool wait_for_queue_drain(int timeout_sec = 60);

    WorkloadProfile profile_;
    std::string     host_;
    uint16_t        data_port_;
    uint16_t        metrics_port_;
};
