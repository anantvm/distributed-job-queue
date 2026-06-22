#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// histogram.hpp
//
// Histogram tracks the distribution of observed values (e.g. latency in ms).
// It maintains per-bucket counters for configurable upper-bound boundaries,
// plus a catch-all +Inf bucket.
//
// HistogramSnapshot is an immutable point-in-time copy that supports
// quantile estimation via linear interpolation.
//
// Thread-safety: Histogram is internally mutex-protected.
// Non-copyable; owned by MetricsRegistry via unique_ptr.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// ─── HistogramSnapshot ───────────────────────────────────────────────────────

struct HistogramSnapshot {
    uint64_t count{0};  // total number of observations
    double   sum{0.0};  // sum of all observed values

    struct Bucket {
        double   upper_bound;        // le label value (+Inf for last bucket)
        uint64_t cumulative_count;   // observations <= upper_bound
    };

    // Buckets are sorted ascending by upper_bound; the last entry is the
    // +Inf bucket (cumulative_count == count).
    std::vector<Bucket> buckets;

    // Estimate the value at the given quantile [0.0, 1.0] using linear
    // interpolation within the enclosing bucket.
    // Returns 0.0 if count == 0. Clamps quantile to [0.0, 1.0].
    [[nodiscard]] double p(double quantile) const;

    // Arithmetic mean of all observations.  Returns 0.0 if count == 0.
    [[nodiscard]] double mean() const;
};

// ─── Histogram ────────────────────────────────────────────────────────────────

class Histogram {
public:
    // boundaries: sorted list of finite upper bounds (e.g. {1,5,10,25,50,100,250,500,1000,5000}).
    // A synthetic +Inf bucket is always appended internally.
    explicit Histogram(std::vector<double> boundaries);

    ~Histogram() = default;

    // Non-copyable, non-movable.
    Histogram(const Histogram&)            = delete;
    Histogram& operator=(const Histogram&) = delete;
    Histogram(Histogram&&)                 = delete;
    Histogram& operator=(Histogram&&)      = delete;

    // Record one observation (e.g. a latency in milliseconds).
    void observe(double value_ms);

    // Return an immutable snapshot of the current state.
    [[nodiscard]] HistogramSnapshot snapshot() const;

    // Reset all counters and sum to zero (for test isolation).
    void reset();

private:
    mutable std::mutex      mu_;
    std::vector<double>     boundaries_;   // finite upper bounds, sorted ascending
    std::vector<uint64_t>   counts_;       // counts_[i] = observations that fall in bucket i
                                           // (NOT cumulative; last slot = +Inf bucket)
    uint64_t                count_{0};     // total observations
    double                  sum_{0.0};     // sum of all observed values
};
