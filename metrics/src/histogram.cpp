// ─────────────────────────────────────────────────────────────────────────────
// histogram.cpp
//
// Implements Histogram::observe, snapshot, reset and HistogramSnapshot::p, mean.
//
// Bucket layout:
//   counts_[0..N-1]  → one slot per finite boundary (value <= boundary[i])
//   counts_[N]       → the +Inf bucket (all observations land here too)
//
// observe(v) increments every bucket whose upper_bound >= v, plus the +Inf bucket.
// This matches Prometheus semantics: each bucket is cumulative over values <= le.
// However we store NON-cumulative per-bucket counts internally for easy reset,
// and make them cumulative only at snapshot() time.
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/histogram.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

// ─── Histogram ────────────────────────────────────────────────────────────────

Histogram::Histogram(std::vector<double> boundaries)
    : boundaries_(std::move(boundaries))
{
    // Ensure boundaries are sorted and finite.
    if (!std::is_sorted(boundaries_.begin(), boundaries_.end())) {
        throw std::invalid_argument("Histogram: boundaries must be sorted ascending");
    }
    for (double b : boundaries_) {
        if (!std::isfinite(b)) {
            throw std::invalid_argument("Histogram: boundaries must be finite");
        }
    }

    // counts_: one slot per finite boundary + one extra slot for the +Inf bucket.
    counts_.assign(boundaries_.size() + 1, 0ULL);
}

void Histogram::observe(double value_ms) {
    std::lock_guard<std::mutex> lk{mu_};

    // Increment each finite-boundary bucket where upper_bound >= value_ms.
    // Using lower_bound to find the first boundary >= value_ms.
    const auto it = std::lower_bound(boundaries_.begin(), boundaries_.end(), value_ms);
    const std::size_t first_idx = static_cast<std::size_t>(
        std::distance(boundaries_.begin(), it));

    // Buckets at indices [first_idx .. boundaries_.size()-1] have upper_bound >= value_ms.
    for (std::size_t i = first_idx; i < boundaries_.size(); ++i) {
        ++counts_[i];
    }

    // Always increment the +Inf bucket (last slot).
    ++counts_[boundaries_.size()];

    ++count_;
    sum_ += value_ms;
}

HistogramSnapshot Histogram::snapshot() const {
    std::lock_guard<std::mutex> lk{mu_};

    HistogramSnapshot snap;
    snap.count = count_;
    snap.sum   = sum_;

    snap.buckets.reserve(boundaries_.size() + 1);

    // Build cumulative counts.
    // counts_[i] is the number of observations that fall exclusively into
    // bucket i (between boundary[i-1] and boundary[i]).  We convert to
    // the Prometheus-style cumulative (number of observations <= boundary[i]).
    //
    // Because observe() increments all buckets with upper_bound >= value,
    // counts_[i] (for finite boundaries) already represents observations <= boundary[i],
    // BUT it's relative to a decreasing set.  Let's think again:
    //
    // Actually observe() increments counts_[i] for every i where boundary[i] >= value.
    // So counts_[0] gets incremented when value <= boundary[0] (smallest bucket).
    // counts_[N-1] (last finite) gets incremented when value <= boundary[N-1].
    // counts_[N] (+Inf) always gets incremented.
    //
    // This means counts_[i] already IS the cumulative count for boundary[i].
    // Just copy them directly.

    uint64_t cumulative = 0;
    for (std::size_t i = 0; i < boundaries_.size(); ++i) {
        snap.buckets.push_back({boundaries_[i], counts_[i]});
        cumulative = counts_[i];  // track for +Inf consistency
    }

    // +Inf bucket: cumulative_count must equal total count.
    snap.buckets.push_back({std::numeric_limits<double>::infinity(), count_});

    (void)cumulative;  // used conceptually above
    return snap;
}

void Histogram::reset() {
    std::lock_guard<std::mutex> lk{mu_};
    std::fill(counts_.begin(), counts_.end(), 0ULL);
    count_ = 0;
    sum_   = 0.0;
}

// ─── HistogramSnapshot ────────────────────────────────────────────────────────

double HistogramSnapshot::mean() const {
    if (count == 0) return 0.0;
    return sum / static_cast<double>(count);
}

double HistogramSnapshot::p(double quantile) const {
    if (count == 0) return 0.0;

    // Clamp to [0.0, 1.0].
    if (quantile <= 0.0) return 0.0;
    if (quantile >= 1.0) {
        // Return the last finite upper bound as the maximum known value.
        if (buckets.size() >= 2) {
            return buckets[buckets.size() - 2].upper_bound;
        }
        return 0.0;
    }

    // Target count: how many observations should fall below the quantile value.
    const double target = static_cast<double>(count) * quantile;

    // Find the first bucket whose cumulative_count >= target.
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        if (buckets[i].cumulative_count == 0) continue;

        if (static_cast<double>(buckets[i].cumulative_count) >= target) {
            // Linear interpolation:
            //   lower bound of this bucket = previous bucket's upper_bound (or 0).
            //   upper bound = this bucket's upper_bound.
            double lower = (i == 0) ? 0.0 : buckets[i - 1].upper_bound;
            double upper = buckets[i].upper_bound;

            // If this is the +Inf bucket, just return the previous upper_bound.
            if (!std::isfinite(upper)) {
                return (i == 0) ? 0.0 : buckets[i - 1].upper_bound;
            }

            uint64_t lower_count = (i == 0) ? 0 : buckets[i - 1].cumulative_count;
            uint64_t upper_count = buckets[i].cumulative_count;

            if (upper_count == lower_count) {
                // Degenerate bucket — just return midpoint.
                return (lower + upper) / 2.0;
            }

            // Fraction of the way through this bucket.
            double frac = (target - static_cast<double>(lower_count)) /
                          static_cast<double>(upper_count - lower_count);

            return lower + frac * (upper - lower);
        }
    }

    // Fallback: return the last finite upper bound.
    if (buckets.size() >= 2) {
        return buckets[buckets.size() - 2].upper_bound;
    }
    return 0.0;
}
