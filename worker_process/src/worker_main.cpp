// worker_process/src/worker_main.cpp
//
// Distributed Worker Process — entry point
//
// Usage:
//   ./job_worker_process [--host 127.0.0.1] [--port 7777]
//
// Registers the same job handlers as Phase 1 and connects to the server.
// Graceful shutdown on SIGINT / SIGTERM.

#include <worker_process/tcp_worker.hpp>
#include <worker/handler_registry.hpp>
#include <common/logger.hpp>
#include <common/job.hpp>

#include <csignal>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

using namespace std::chrono_literals;

static std::atomic<bool> g_stop{false};
static TcpWorker*        g_worker_ptr = nullptr;

static void sighandler(int) {
    g_stop = true;
    if (g_worker_ptr) g_worker_ptr->stop();
}

static std::string get_arg(int argc, char** argv,
                            const std::string& flag, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return def;
}

// ─── Job handlers (same as Phase 1 demo) ─────────────────────────────────────

static uint64_t fibonacci(int n) {
    if (n <= 1) return static_cast<uint64_t>(n);
    uint64_t a = 0, b = 1;
    for (int i = 2; i <= n; ++i) { uint64_t c = a + b; a = b; b = c; }
    return b;
}

static void sleep_handler(const Job& job) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(job.payload)));
}

static void fibonacci_handler(const Job& job) {
    auto r = fibonacci(std::stoi(job.payload));
    (void)r;
}

static void flaky_handler(const Job& job) {
    if (job.retry_count < 1)
        throw std::runtime_error("Simulated transient failure");
    std::this_thread::sleep_for(10ms);
}

static void always_fail_handler(const Job&) {
    throw std::runtime_error("Always fails");
}

int main(int argc, char** argv) {
    Logger::set_level(LogLevel::INFO);

    const std::string host = get_arg(argc, argv, "--host", "127.0.0.1");
    const uint16_t    port = static_cast<uint16_t>(
                                 std::stoi(get_arg(argc, argv, "--port", "7777")));

    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    HandlerRegistry registry;
    registry.register_handler("sleep_job",       sleep_handler);
    registry.register_handler("fibonacci_job",   fibonacci_handler);
    registry.register_handler("flaky_job",       flaky_handler);
    registry.register_handler("always_fail_job", always_fail_handler);

    std::cout << "\033[1;34m"
              << "╔═══════════════════════════════════════╗\n"
              << "║   Distributed Job Queue — Phase 2     ║\n"
              << "║   Worker Process                      ║\n"
              << "╚═══════════════════════════════════════╝\n"
              << "\033[0m"
              << "  Server : " << host << ":" << port << "\n\n";

    TcpWorker worker(host, port, registry);
    g_worker_ptr = &worker;

    worker.run();  // blocks until SIGINT or server shutdown

    const auto s = worker.stats();
    std::cout << "\n  Worker stats:\n"
              << "    Executed  : " << s.jobs_executed  << "\n"
              << "    Succeeded : " << s.jobs_succeeded << "\n"
              << "    Failed    : " << s.jobs_failed    << "\n";

    std::cout << "\033[1;32m✔ Worker exited cleanly.\033[0m\n";
    return EXIT_SUCCESS;
}
