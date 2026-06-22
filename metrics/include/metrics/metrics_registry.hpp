#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// metrics_registry.hpp
//
// Process-level singleton that owns all Counters, Gauges, and Histograms.
//
// Design:
//   • Meyer's singleton — thread-safe in C++11+ due to magic static guarantee.
//   • Metrics are created lazily on first request and reused on subsequent calls.
//   • std::shared_mutex enables concurrent reads (prometheus_text, value queries)
//     while serialising writes (first-time registration).
//   • Metrics are named following Prometheus snake_case conventions.
//   • prometheus_text() renders the full /metrics Prometheus exposition format.
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/counter.hpp>
#include <metrics/gauge.hpp>
#include <metrics/histogram.hpp>

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class MetricsRegistry {
public:
    // ── Singleton access ──────────────────────────────────────────────────────
    // Meyer's singleton: initialised exactly once, thread-safe per C++11.
    static MetricsRegistry& instance();

    // Non-copyable, non-movable.
    MetricsRegistry(const MetricsRegistry&)            = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;
    MetricsRegistry(MetricsRegistry&&)                 = delete;
    MetricsRegistry& operator=(MetricsRegistry&&)      = delete;

    // ── Metric registration / lookup ──────────────────────────────────────────

    // Returns the Counter registered under `name`.  Creates it on first call.
    // If the name already exists, `help` is ignored and the existing metric is
    // returned — callers may safely call counter() many times with the same name.
    Counter& counter(const std::string& name, const std::string& help = "");

    // Returns the Gauge registered under `name`.  Creates on first call.
    Gauge& gauge(const std::string& name, const std::string& help = "");

    // Returns the Histogram registered under `name`.  Creates on first call.
    // `boundaries` must be sorted ascending finite values (milliseconds).
    Histogram& histogram(
        const std::string&         name,
        const std::string&         help       = "",
        std::vector<double>        boundaries = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000});

    // ── Prometheus exposition ─────────────────────────────────────────────────

    // Serialise all registered metrics to the Prometheus text exposition format
    // (version 0.0.4).  Suitable for serving at GET /metrics.
    [[nodiscard]] std::string prometheus_text() const;

    // ── Test support ──────────────────────────────────────────────────────────

    // Reset all metric values to zero.  Does NOT remove registrations.
    void reset_all();

private:
    MetricsRegistry() = default;
    ~MetricsRegistry() = default;

    // ── Internal storage ──────────────────────────────────────────────────────

    mutable std::shared_mutex mu_;

    struct CounterEntry {
        std::unique_ptr<Counter> metric;
        std::string              help;
    };
    struct GaugeEntry {
        std::unique_ptr<Gauge>   metric;
        std::string              help;
    };
    struct HistogramEntry {
        std::unique_ptr<Histogram> metric;
        std::string                help;
    };

    std::unordered_map<std::string, CounterEntry>   counters_;
    std::unordered_map<std::string, GaugeEntry>     gauges_;
    std::unordered_map<std::string, HistogramEntry> histograms_;

    // Insertion order tracking for stable prometheus_text() output.
    std::vector<std::string> counter_order_;
    std::vector<std::string> gauge_order_;
    std::vector<std::string> histogram_order_;
};
