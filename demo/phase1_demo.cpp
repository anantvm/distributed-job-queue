// demo/phase1_demo.cpp
//
// ═══════════════════════════════════════════════════════════════════════════════
// High-Performance Distributed Job Queue — Phase 1 Integration Demo
// ═══════════════════════════════════════════════════════════════════════════════
//
// This program demonstrates all Phase 1 capabilities in a single process:
//
//  1. Creating a SQLite-backed JobManager.
//  2. Startup recovery (loads any PENDING/RUNNING jobs from a previous run).
//  3. Submitting jobs with different priorities (HIGH > NORMAL > LOW).
//  4. Running 4 concurrent worker threads via std::jthread.
//  5. Showing priority ordering (HIGH jobs execute before LOW ones).
//  6. Demonstrating the retry mechanism (intentionally failing jobs).
//  7. Printing a final metrics summary.
//
// Run it multiple times against the same jobs.db to observe recovery.

#include <common/job.hpp>
#include <common/logger.hpp>
#include <manager/job_manager.hpp>
#include <storage/sqlite_backend.hpp>
#include <worker/handler_registry.hpp>
#include <worker/worker.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Job Handlers ─────────────────────────────────────────────────────────────

// Simulates a CPU-bound task: iterative Fibonacci.
static uint64_t fibonacci(int n) {
    if (n <= 1) return static_cast<uint64_t>(n);
    uint64_t a = 0, b = 1;
    for (int i = 2; i <= n; ++i) { uint64_t c = a + b; a = b; b = c; }
    return b;
}

// Simulates an I/O-bound task by sleeping.
static void sleep_handler(const Job& job) {
    const int ms = std::stoi(job.payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Lightweight CPU work.
static void fibonacci_handler(const Job& job) {
    const int n   = std::stoi(job.payload);
    const auto result = fibonacci(n);
    (void)result;  // suppress unused-variable warning
}

// Always throws — used to exercise the retry and failure path.
static void flaky_handler(const Job& job) {
    // Fail on the first attempt only (retry_count == 0).
    if (job.retry_count < 1) {
        throw std::runtime_error("Simulated transient failure on attempt 1");
    }
    // Succeed on second attempt.
    std::this_thread::sleep_for(10ms);
}

// Always fails permanently — exercises the dead-letter path.
static void always_fail_handler(const Job& /*job*/) {
    throw std::runtime_error("This job always fails (testing DLQ path)");
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void print_separator(const std::string& title) {
    std::cout << "\n\033[1;34m";
    std::cout << "══════════════════════════════════════════\n";
    std::cout << "  " << title << "\n";
    std::cout << "══════════════════════════════════════════\033[0m\n\n";
}

static void wait_for_completion(
    JobManager& manager,
    uint64_t    expected_terminal,
    std::chrono::seconds timeout = 30s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto m = manager.get_metrics();
        const uint64_t terminal = m.jobs_completed + m.jobs_failed;
        if (terminal >= expected_terminal) return;
        std::this_thread::sleep_for(50ms);
    }
    Logger::warn("Demo", "Timed out waiting for jobs to complete");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    Logger::set_level(LogLevel::INFO);

    print_separator("Phase 1 — Single-Node Job Queue");

    // ── 1. Storage backend ────────────────────────────────────────────────────
    // "jobs.db" persists across runs; delete the file to start fresh.
    const std::string db_path = "jobs.db";
    Logger::info("Demo", "Using SQLite database: " + db_path);

    auto storage = std::make_unique<SQLiteBackend>(db_path);
    auto manager = std::make_unique<JobManager>(std::move(storage));

    // ── 2. Recover jobs from a previous run ────────────────────────────────────
    if (auto r = manager->initialize(); r.err()) {
        Logger::error("Demo", "Failed to initialize manager: " + r.error());
        return EXIT_FAILURE;
    }

    // ── 3. Handler registry ────────────────────────────────────────────────────
    HandlerRegistry registry;
    registry.register_handler("sleep_job",       sleep_handler);
    registry.register_handler("fibonacci_job",   fibonacci_handler);
    registry.register_handler("flaky_job",       flaky_handler);
    registry.register_handler("always_fail_job", always_fail_handler);

    Logger::info("Demo", "Registered " + std::to_string(registry.size()) + " handlers");

    // ── 4. Submit jobs ─────────────────────────────────────────────────────────
    print_separator("Submitting Jobs");

    const int NUM_HIGH   = 3;
    const int NUM_NORMAL = 5;
    const int NUM_LOW    = 2;
    const int NUM_FLAKY  = 2;  // will retry once
    const int NUM_ALWAYS_FAIL = 1;  // will exhaust retries

    const int TOTAL_JOBS = NUM_HIGH + NUM_NORMAL + NUM_LOW + NUM_FLAKY + NUM_ALWAYS_FAIL;

    // HIGH priority — fibonacci, CPU-bound
    for (int i = 0; i < NUM_HIGH; ++i) {
        auto r = manager->submit_job("fibonacci_job", "40", Priority::HIGH, 1);
        if (r.err()) Logger::error("Demo", "Submit error: " + r.error());
    }

    // NORMAL priority — short sleep, I/O-bound simulation
    for (int i = 0; i < NUM_NORMAL; ++i) {
        auto r = manager->submit_job("sleep_job", "80", Priority::NORMAL, 1);
        if (r.err()) Logger::error("Demo", "Submit error: " + r.error());
    }

    // LOW priority — sleep
    for (int i = 0; i < NUM_LOW; ++i) {
        auto r = manager->submit_job("sleep_job", "50", Priority::LOW, 1);
        if (r.err()) Logger::error("Demo", "Submit error: " + r.error());
    }

    // FLAKY — fails once, retries, then succeeds (max_retries=2)
    for (int i = 0; i < NUM_FLAKY; ++i) {
        auto r = manager->submit_job("flaky_job", "", Priority::NORMAL, 2);
        if (r.err()) Logger::error("Demo", "Submit error: " + r.error());
    }

    // ALWAYS FAIL — exhausts retries → permanently FAILED
    for (int i = 0; i < NUM_ALWAYS_FAIL; ++i) {
        auto r = manager->submit_job("always_fail_job", "", Priority::NORMAL, 2);
        if (r.err()) Logger::error("Demo", "Submit error: " + r.error());
    }

    {
        auto m = manager->get_metrics();
        Logger::info("Demo",
            "Submitted " + std::to_string(m.jobs_submitted) +
            " jobs  |  Queue depth: " + std::to_string(m.queue_length));
    }

    // ── 5. Start worker threads ────────────────────────────────────────────────
    print_separator("Starting Workers");

    const int NUM_WORKERS = 4;
    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(NUM_WORKERS);

    for (int i = 1; i <= NUM_WORKERS; ++i) {
        workers.push_back(std::make_unique<Worker>(i, *manager, registry));
        workers.back()->start();
    }
    Logger::info("Demo", std::to_string(NUM_WORKERS) + " workers running");

    // ── 6. Wait for all terminal states ───────────────────────────────────────
    print_separator("Processing");

    // Expected terminal = TOTAL_JOBS (each job ends in COMPLETED or FAILED).
    // Flaky jobs retry (so more executions than jobs), but only one terminal.
    wait_for_completion(*manager, static_cast<uint64_t>(TOTAL_JOBS));

    // ── 7. Graceful shutdown ───────────────────────────────────────────────────
    print_separator("Shutdown");

    manager->shutdown();

    // Workers' jthread destructors will join automatically; explicitly stop
    // to get clean log ordering.
    for (auto& w : workers) {
        w->stop();
    }

    // ── 8. Final metrics ───────────────────────────────────────────────────────
    print_separator("Final Metrics");

    const auto m = manager->get_metrics();

    std::cout << "  Jobs submitted  : " << m.jobs_submitted  << "\n";
    std::cout << "  Jobs completed  : " << m.jobs_completed  << "\n";
    std::cout << "  Jobs failed     : " << m.jobs_failed     << "\n";
    std::cout << "  Jobs retried    : " << m.jobs_retried    << "\n";
    std::cout << "  Queue remaining : " << m.queue_length    << "\n\n";

    std::cout << "  Per-worker stats:\n";
    for (const auto& w : workers) {
        const auto s = w->stats();
        std::cout << "    Worker-" << w->id()
                  << "  executed=" << s.jobs_executed
                  << "  ok="       << s.jobs_succeeded
                  << "  fail="     << s.jobs_failed
                  << "  total_ms=" << s.total_execution_time.count() << "\n";
    }

    std::cout << "\n\033[1;32m✔ Phase 1 complete.\033[0m\n\n";
    return EXIT_SUCCESS;
}
