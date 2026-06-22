#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// metrics_client.hpp — Prometheus text-format poller over raw TCP
//
// MetricsClient opens a plain TCP socket, sends an HTTP/1.0 GET request to
// /metrics, reads the response, and parses the Prometheus exposition format
// line by line.  It is intentionally dependency-free (no libcurl, no Boost).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <unordered_map>

// ─── MetricsSnapshot ─────────────────────────────────────────────────────────

struct MetricsSnapshot {
    int64_t timestamp_ms{0};

    /// TYPE counter metrics (monotonically increasing)
    std::unordered_map<std::string, double> counters;

    /// TYPE gauge metrics + derived histogram observations
    /// Histogram percentiles are stored as e.g. "job_e2e_ms_p50", "job_e2e_ms_p99"
    std::unordered_map<std::string, double> gauges;

    /// Convenience: look up a value from either map; returns 0.0 if not found.
    [[nodiscard]] double get(const std::string& name) const noexcept;
};

// ─── MetricsClient ───────────────────────────────────────────────────────────

class MetricsClient {
public:
    MetricsClient(std::string host, uint16_t metrics_port);

    /// Perform one HTTP GET /metrics and return a parsed snapshot.
    [[nodiscard]] MetricsSnapshot poll();

    /// Return true if the metrics endpoint is currently reachable.
    [[nodiscard]] bool is_reachable();

private:
    /// Blocking HTTP GET /metrics over a fresh TCP connection.
    /// Returns the full response body (everything after the blank header line).
    [[nodiscard]] std::string http_get();

    /// Parse Prometheus text exposition format into a MetricsSnapshot.
    [[nodiscard]] MetricsSnapshot parse(const std::string& body,
                                        int64_t             timestamp_ms);

    std::string host_;
    uint16_t    port_;
};
