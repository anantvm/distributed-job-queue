// ─────────────────────────────────────────────────────────────────────────────
// load_generator.cpp
//
// Implements:
//   • WorkloadProfile factory methods (baseline / sustained_load / burst_spike /
//     priority_storm / large_payload)
//   • LoadGenerator: multi-threaded, token-bucket (sleep-based) submit loop
//
// Design notes
// ────────────
// • Each thread has its own JobClient so there is zero socket sharing.
// • Token bucket is implemented as a "next permitted submit" time-point.
//   After each submission the thread computes when the next one is due and
//   sleeps for the remaining interval.  This keeps average throughput tight
//   without busy-spinning.
// • Per-thread rate = profile_.submit_rate_rps / num_submit_threads.
// • Job-type and Priority are sampled from weighted distributions using a
//   prefix-sum + uniform draw (alias-free, O(n)).
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/load_generator.hpp>

#include <client/job_client.hpp>
#include <common/logger.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace std::chrono;

// ═══════════════════════════════════════════════════════════════════════════════
// WorkloadProfile factory methods
// ═══════════════════════════════════════════════════════════════════════════════

WorkloadProfile WorkloadProfile::baseline() {
    WorkloadProfile p;
    p.name                = "baseline";
    p.duration_sec        = 60;
    p.submit_rate_rps     = 50.0;
    p.num_submit_threads  = 2;
    p.num_workers         = 2;
    p.payload_size_bytes  = 64;
    p.max_retries         = 3;
    p.job_type_mix        = {{"benchmark_job", 1.0}};
    p.priority_mix        = {
        {Priority::LOW,    0.1},
        {Priority::NORMAL, 0.8},
        {Priority::HIGH,   0.1},
    };
    return p;
}

WorkloadProfile WorkloadProfile::sustained_load() {
    WorkloadProfile p;
    p.name                = "sustained_load";
    p.duration_sec        = 120;
    p.submit_rate_rps     = 300.0;
    p.num_submit_threads  = 6;
    p.num_workers         = 4;
    p.payload_size_bytes  = 128;
    p.max_retries         = 3;
    p.job_type_mix        = {
        {"compute_job", 0.5},
        {"io_job",      0.3},
        {"email_job",   0.2},
    };
    p.priority_mix        = {
        {Priority::LOW,    0.2},
        {Priority::NORMAL, 0.6},
        {Priority::HIGH,   0.2},
    };
    return p;
}

WorkloadProfile WorkloadProfile::burst_spike() {
    WorkloadProfile p;
    p.name                = "burst_spike";
    p.duration_sec        = 30;
    p.submit_rate_rps     = 1000.0;
    p.num_submit_threads  = 8;
    p.num_workers         = 4;
    p.payload_size_bytes  = 64;
    p.max_retries         = 1;
    p.job_type_mix        = {{"benchmark_job", 1.0}};
    p.priority_mix        = {
        {Priority::LOW,    0.05},
        {Priority::NORMAL, 0.85},
        {Priority::HIGH,   0.10},
    };
    return p;
}

WorkloadProfile WorkloadProfile::priority_storm() {
    WorkloadProfile p;
    p.name                = "priority_storm";
    p.duration_sec        = 60;
    p.submit_rate_rps     = 500.0;
    p.num_submit_threads  = 8;
    p.num_workers         = 4;
    p.payload_size_bytes  = 64;
    p.max_retries         = 3;
    p.job_type_mix        = {
        {"high_priority_job", 0.7},
        {"benchmark_job",     0.3},
    };
    p.priority_mix        = {
        {Priority::LOW,    0.0},
        {Priority::NORMAL, 0.1},
        {Priority::HIGH,   0.9},
    };
    return p;
}

WorkloadProfile WorkloadProfile::large_payload() {
    WorkloadProfile p;
    p.name                = "large_payload";
    p.duration_sec        = 60;
    p.submit_rate_rps     = 50.0;
    p.num_submit_threads  = 4;
    p.num_workers         = 4;
    p.payload_size_bytes  = 65536;  // 64 KiB
    p.max_retries         = 3;
    p.job_type_mix        = {{"blob_job", 1.0}};
    p.priority_mix        = {
        {Priority::LOW,    0.2},
        {Priority::NORMAL, 0.6},
        {Priority::HIGH,   0.2},
    };
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════════
// LoadGenerator
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// Current wall-clock time in milliseconds since epoch.
inline int64_t now_ms() {
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

// Current wall-clock time as a steady_clock time_point.
inline steady_clock::time_point now_tp() {
    return steady_clock::now();
}

} // anonymous namespace

// ── Constructor ───────────────────────────────────────────────────────────────

LoadGenerator::LoadGenerator(WorkloadProfile profile,
                             std::string     host,
                             uint16_t        port)
    : profile_(std::move(profile))
    , host_(std::move(host))
    , port_(port) {}

// ── start / stop ─────────────────────────────────────────────────────────────

void LoadGenerator::start() {
    if (running_.exchange(true)) return;  // already running

    submitted_.store(0, std::memory_order_relaxed);
    rejected_.store(0,  std::memory_order_relaxed);
    errors_.store(0,    std::memory_order_relaxed);

    // Capture wall-clock start.
    start_time_ms_ = now_ms();

    Logger::info("LoadGenerator",
                 "Starting " + std::to_string(profile_.num_submit_threads) +
                 " submit threads at " +
                 std::to_string(static_cast<int>(profile_.submit_rate_rps)) +
                 " rps for " + std::to_string(profile_.duration_sec) + "s");

    threads_.clear();
    threads_.reserve(static_cast<size_t>(profile_.num_submit_threads));

    for (int i = 0; i < profile_.num_submit_threads; ++i) {
        threads_.emplace_back([this, i]() { submit_loop(i); });
    }
}

void LoadGenerator::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    Logger::info("LoadGenerator", "All submit threads stopped.");
}

// ── stats ─────────────────────────────────────────────────────────────────────

LoadStats LoadGenerator::stats() const {
    LoadStats s;
    s.jobs_submitted = submitted_.load(std::memory_order_relaxed);
    s.jobs_rejected  = rejected_.load(std::memory_order_relaxed);
    s.submit_errors  = errors_.load(std::memory_order_relaxed);
    s.elapsed_ms     = now_ms() - start_time_ms_;

    double elapsed_sec = static_cast<double>(s.elapsed_ms) / 1000.0;
    if (elapsed_sec > 0.0) {
        s.actual_rps = static_cast<double>(s.jobs_submitted + s.jobs_rejected)
                       / elapsed_sec;
    }
    return s;
}

// ── submit_loop ───────────────────────────────────────────────────────────────

void LoadGenerator::submit_loop(int thread_id) {
    // Seed the RNG with time + thread_id for uniqueness.
    const unsigned seed =
        static_cast<unsigned>(
            duration_cast<microseconds>(
                steady_clock::now().time_since_epoch())
            .count()) ^
        (static_cast<unsigned>(thread_id) * 2654435761u);

    std::mt19937 rng{seed};

    // Create a per-thread client.
    JobClient client(host_, port_);

    // Connect; retry once if the first attempt fails.
    {
        auto r = client.connect();
        if (r.err()) {
            Logger::warn("LoadGenerator",
                         "Thread " + std::to_string(thread_id) +
                         " initial connect failed: " + r.error() +
                         " — retrying in 1s");
            std::this_thread::sleep_for(seconds(1));
            r = client.connect();
            if (r.err()) {
                Logger::error("LoadGenerator",
                              "Thread " + std::to_string(thread_id) +
                              " connect permanently failed: " + r.error());
                errors_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // Per-thread rate = total_rate / num_threads.
    const int    n_threads = std::max(1, profile_.num_submit_threads);
    const double per_thread_rate =
        profile_.submit_rate_rps / static_cast<double>(n_threads);

    // Interval between submissions in microseconds.
    const int64_t interval_us =
        static_cast<int64_t>(1'000'000.0 / per_thread_rate);

    auto next_submit = now_tp();

    while (running_.load(std::memory_order_acquire)) {
        // Throttle: sleep until next scheduled submit.
        auto now = now_tp();
        if (now < next_submit) {
            std::this_thread::sleep_until(next_submit);
        }
        // Advance next_submit regardless of how late we woke up.
        next_submit += microseconds(interval_us);

        if (!running_.load(std::memory_order_relaxed)) break;

        // Sample distributions.
        const std::string job_type = pick_job_type(rng);
        const Priority    priority = pick_priority(rng);
        const std::string payload  = make_payload(rng);

        // Submit.
        auto result = client.submit(job_type, payload, priority,
                                    profile_.max_retries);

        if (result.ok()) {
            submitted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Check if it's a connection-level error or a server rejection.
            const std::string& err_str = result.error();
            bool is_transport =
                err_str.find("connect") != std::string::npos ||
                err_str.find("send")    != std::string::npos ||
                err_str.find("recv")    != std::string::npos ||
                err_str.find("socket")  != std::string::npos;

            if (is_transport) {
                errors_.fetch_add(1, std::memory_order_relaxed);
                Logger::warn("LoadGenerator",
                             "Thread " + std::to_string(thread_id) +
                             " transport error: " + err_str +
                             " — reconnecting");
                client.disconnect();
                std::this_thread::sleep_for(milliseconds(50));
                auto rc = client.connect();
                if (rc.err()) {
                    Logger::error("LoadGenerator",
                                  "Thread " + std::to_string(thread_id) +
                                  " reconnect failed: " + rc.error());
                }
            } else {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                Logger::debug("LoadGenerator",
                              "Thread " + std::to_string(thread_id) +
                              " job rejected: " + err_str);
            }
        }
    }

    client.disconnect();
}

// ── Weighted random helpers ───────────────────────────────────────────────────

std::string LoadGenerator::pick_job_type(std::mt19937& rng) const {
    const auto& mix = profile_.job_type_mix;
    if (mix.empty()) return "benchmark_job";
    if (mix.size() == 1) return mix[0].job_type;

    double total = 0.0;
    for (const auto& m : mix) total += m.weight;

    std::uniform_real_distribution<double> dist(0.0, total);
    double draw = dist(rng);
    double cum  = 0.0;
    for (const auto& m : mix) {
        cum += m.weight;
        if (draw <= cum) return m.job_type;
    }
    return mix.back().job_type;
}

Priority LoadGenerator::pick_priority(std::mt19937& rng) const {
    const auto& mix = profile_.priority_mix;
    if (mix.empty()) return Priority::NORMAL;
    if (mix.size() == 1) return mix[0].priority;

    double total = 0.0;
    for (const auto& m : mix) total += m.weight;

    std::uniform_real_distribution<double> dist(0.0, total);
    double draw = dist(rng);
    double cum  = 0.0;
    for (const auto& m : mix) {
        cum += m.weight;
        if (draw <= cum) return m.priority;
    }
    return mix.back().priority;
}

std::string LoadGenerator::make_payload(std::mt19937& rng) const {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    const int len = std::max(1, profile_.payload_size_bytes);
    std::string s;
    s.reserve(static_cast<size_t>(len));

    std::uniform_int_distribution<size_t> dist(0, kAlphabet.size() - 1);
    for (int i = 0; i < len; ++i) {
        s += kAlphabet[dist(rng)];
    }
    return s;
}
