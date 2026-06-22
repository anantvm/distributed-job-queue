#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// benchmark_report.hpp — Aggregated results from one benchmark run
//
// BenchmarkReport is a pure-value type: all heavy computation (latency
// percentiles, throughput) has already been resolved by BenchmarkRunner before
// the struct is constructed.  The three output methods produce human-readable
// ASCII table, machine-readable CSV, and structured JSON — all without any
// external formatting library.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

struct BenchmarkReport {
    // ── Identity ──────────────────────────────────────────────────────────────
    std::string profile_name;

    // ── Duration ──────────────────────────────────────────────────────────────
    int64_t duration_actual_ms{0};

    // ── Job counts ────────────────────────────────────────────────────────────
    uint64_t jobs_submitted{0};
    uint64_t jobs_completed{0};
    uint64_t jobs_failed{0};

    // ── Throughput ────────────────────────────────────────────────────────────
    double throughput_rps{0.0};  ///< completed / elapsed_sec
    double submit_rps{0.0};      ///< submitted / elapsed_sec

    // ── Queue depth ───────────────────────────────────────────────────────────
    double  queue_depth_avg{0.0};
    int64_t queue_depth_max{0};

    // ── End-to-end latency (server-reported) ──────────────────────────────────
    double e2e_latency_p50_ms{0.0};
    double e2e_latency_p99_ms{0.0};

    // ── Dispatch latency (server-reported) ────────────────────────────────────
    double dispatch_latency_p50_ms{0.0};
    double dispatch_latency_p99_ms{0.0};

    // ── Time series (1-second granularity) ────────────────────────────────────
    struct TimeSeries {
        int64_t ts_ms{0};
        double  rps{0.0};
        int64_t queue_depth{0};
        double  e2e_p99_ms{0.0};
    };
    std::vector<TimeSeries> series;

    // ── Output methods ────────────────────────────────────────────────────────

    /// Unicode box-drawing table (UTF-8).
    [[nodiscard]] std::string to_table_string() const;

    /// CSV with one header row + one data row per time-series entry.
    [[nodiscard]] std::string to_csv_string() const;

    /// Compact JSON object (no external library).
    [[nodiscard]] std::string to_json_string() const;

    /// Print the table + separator to stdout.
    void print() const;
};
