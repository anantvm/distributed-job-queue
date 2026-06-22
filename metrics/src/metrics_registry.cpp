// ─────────────────────────────────────────────────────────────────────────────
// metrics_registry.cpp
//
// MetricsRegistry singleton implementation.
//
// Thread-safety model:
//   • Reads (prometheus_text, value queries via returned refs) → shared_lock.
//   • First-time creation of a metric → upgrade to unique_lock.
//   • The "check-then-insert" pattern uses a shared_lock for the fast path
//     and a unique_lock for the slow (first-registration) path.
//
// Note: returned Counter/Gauge/Histogram references are stable for the process
// lifetime (unique_ptr in map, map never erases entries after insertion).
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/metrics_registry.hpp>

#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>

// ─── Singleton ────────────────────────────────────────────────────────────────

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry inst;
    return inst;
}

// ─── counter() ────────────────────────────────────────────────────────────────

Counter& MetricsRegistry::counter(const std::string& name, const std::string& help) {
    // Fast path: already exists → shared lock only.
    {
        std::shared_lock<std::shared_mutex> rl{mu_};
        auto it = counters_.find(name);
        if (it != counters_.end()) {
            return *it->second.metric;
        }
    }
    // Slow path: first registration → exclusive lock.
    std::unique_lock<std::shared_mutex> wl{mu_};
    // Re-check under exclusive lock (another thread may have raced us).
    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return *it->second.metric;
    }
    auto& entry = counters_[name];
    entry.metric = std::make_unique<Counter>();
    entry.help   = help;
    counter_order_.push_back(name);
    return *entry.metric;
}

// ─── gauge() ──────────────────────────────────────────────────────────────────

Gauge& MetricsRegistry::gauge(const std::string& name, const std::string& help) {
    {
        std::shared_lock<std::shared_mutex> rl{mu_};
        auto it = gauges_.find(name);
        if (it != gauges_.end()) {
            return *it->second.metric;
        }
    }
    std::unique_lock<std::shared_mutex> wl{mu_};
    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return *it->second.metric;
    }
    auto& entry = gauges_[name];
    entry.metric = std::make_unique<Gauge>();
    entry.help   = help;
    gauge_order_.push_back(name);
    return *entry.metric;
}

// ─── histogram() ──────────────────────────────────────────────────────────────

Histogram& MetricsRegistry::histogram(
    const std::string& name,
    const std::string& help,
    std::vector<double> boundaries)
{
    {
        std::shared_lock<std::shared_mutex> rl{mu_};
        auto it = histograms_.find(name);
        if (it != histograms_.end()) {
            return *it->second.metric;
        }
    }
    std::unique_lock<std::shared_mutex> wl{mu_};
    auto it = histograms_.find(name);
    if (it != histograms_.end()) {
        return *it->second.metric;
    }
    auto& entry = histograms_[name];
    entry.metric = std::make_unique<Histogram>(std::move(boundaries));
    entry.help   = help;
    histogram_order_.push_back(name);
    return *entry.metric;
}

// ─── prometheus_text() ────────────────────────────────────────────────────────
//
// Prometheus text exposition format (version 0.0.4):
//   # HELP <name> <help text>
//   # TYPE <name> counter|gauge|histogram
//   <name>[{labels}] <value>
//
// For histograms the format is:
//   <name>_bucket{le="<bound>"} <cumulative_count>
//   <name>_sum <sum>
//   <name>_count <count>

static std::string format_double(double v) {
    if (std::isinf(v)) return "+Inf";
    if (std::isnan(v)) return "NaN";
    // Strip trailing zeros for cleaner output.
    std::ostringstream oss;
    oss << std::setprecision(10) << v;
    return oss.str();
}

std::string MetricsRegistry::prometheus_text() const {
    std::shared_lock<std::shared_mutex> rl{mu_};

    std::ostringstream out;

    // ── Counters ──────────────────────────────────────────────────────────────
    for (const auto& name : counter_order_) {
        auto it = counters_.find(name);
        if (it == counters_.end()) continue;
        const auto& entry = it->second;

        if (!entry.help.empty()) {
            out << "# HELP " << name << " " << entry.help << "\n";
        }
        out << "# TYPE " << name << " counter\n";
        out << name << " " << entry.metric->value() << "\n";
    }

    // ── Gauges ────────────────────────────────────────────────────────────────
    for (const auto& name : gauge_order_) {
        auto it = gauges_.find(name);
        if (it == gauges_.end()) continue;
        const auto& entry = it->second;

        if (!entry.help.empty()) {
            out << "# HELP " << name << " " << entry.help << "\n";
        }
        out << "# TYPE " << name << " gauge\n";
        out << name << " " << entry.metric->value() << "\n";
    }

    // ── Histograms ────────────────────────────────────────────────────────────
    for (const auto& name : histogram_order_) {
        auto it = histograms_.find(name);
        if (it == histograms_.end()) continue;
        const auto& entry = it->second;

        if (!entry.help.empty()) {
            out << "# HELP " << name << " " << entry.help << "\n";
        }
        out << "# TYPE " << name << " histogram\n";

        // snapshot() acquires the histogram's internal mutex.
        // We hold only the registry shared_lock here, which is fine because
        // histogram's own mutex is a different lock.
        HistogramSnapshot snap = entry.metric->snapshot();

        for (const auto& bucket : snap.buckets) {
            out << name << "_bucket{le=\""
                << format_double(bucket.upper_bound)
                << "\"} "
                << bucket.cumulative_count
                << "\n";
        }
        out << name << "_sum "   << format_double(snap.sum)   << "\n";
        out << name << "_count " << snap.count                 << "\n";
    }

    return out.str();
}

// ─── reset_all() ──────────────────────────────────────────────────────────────

void MetricsRegistry::reset_all() {
    std::shared_lock<std::shared_mutex> rl{mu_};

    for (auto& [name, entry] : counters_) {
        entry.metric->reset();
    }
    for (auto& [name, entry] : gauges_) {
        entry.metric->set(0);
    }
    for (auto& [name, entry] : histograms_) {
        entry.metric->reset();
    }
}
