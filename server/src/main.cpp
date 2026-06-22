// server/src/main.cpp
//
// Job Manager Server — entry point
//
// Usage:
//   ./job_manager_server [--port 7777] [--metrics-port 7778] [--db jobs.db]
//
// Graceful shutdown on SIGINT / SIGTERM.

#include <server/server.hpp>
#include <storage/sqlite_backend.hpp>
#include <common/logger.hpp>

// Phase 4: observability (optional — compiles without metrics_lib too)
#if __has_include(<metrics/metrics_registry.hpp>)
#  include <metrics/metrics_registry.hpp>
#  include <metrics/metrics_reporter.hpp>
#  include <metrics/http_metrics_server.hpp>
#  include <metrics/latency_tracker.hpp>
#  define METRICS_ENABLED 1
#else
#  define METRICS_ENABLED 0
#endif

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <atomic>

static std::atomic<bool> g_stop{false};
static Server*           g_server_ptr = nullptr;

static void sighandler(int /*sig*/) {
    g_stop = true;
    if (g_server_ptr) g_server_ptr->stop();
}

static std::string get_arg(int argc, char** argv,
                            const std::string& flag, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return def;
}

int main(int argc, char** argv) {
    Logger::set_level(LogLevel::INFO);

    const std::string db_path      = get_arg(argc, argv, "--db",           "jobs.db");
    const uint16_t    port         = static_cast<uint16_t>(
                                         std::stoi(get_arg(argc, argv, "--port",         "7777")));
    const uint16_t    metrics_port = static_cast<uint16_t>(
                                         std::stoi(get_arg(argc, argv, "--metrics-port", "7778")));

    std::signal(SIGINT,  sighandler);
    std::signal(SIGTERM, sighandler);

    std::cout << "\033[1;34m"
              << "╔════════════════════════════════════════╗\n"
              << "║   Distributed Job Queue — Phase 4      ║\n"
              << "║   Job Manager Server                   ║\n"
              << "╚════════════════════════════════════════╝\n"
              << "\033[0m"
              << "  Port         : " << port         << "\n"
              << "  Metrics port : " << metrics_port << "\n"
              << "  DB           : " << db_path      << "\n\n";

    try {
        auto storage = std::make_unique<SQLiteBackend>(db_path);
        Server server(port, std::move(storage));
        g_server_ptr = &server;

#if METRICS_ENABLED
        // Wire metrics: reporter polls JobManager state every 5s;
        // HTTP server exposes /metrics on the metrics port.
        MetricsReporter   reporter(5000);
        HttpMetricsServer http_metrics(metrics_port);

        reporter.set_metrics_fn([&server]() -> JobManagerMetrics {
            auto m = server.get_metrics();
            return {
                m.queue_length,
                m.jobs_submitted,
                m.jobs_completed,
                m.jobs_failed,
                m.jobs_retried,
                m.active_workers,
                m.active_leases
            };
        });
        reporter.set_purge_fn([]() {
            using namespace std::chrono;
            int64_t cutoff = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count() - 300'000;
            LatencyTracker::instance().purge_older_than(cutoff);
        });

        reporter.start();
        http_metrics.start();
        Logger::info("main", "Metrics HTTP server on port " + std::to_string(metrics_port));
#endif

        server.run();  // blocks until SIGINT

#if METRICS_ENABLED
        http_metrics.stop();
        reporter.stop();
#endif

    } catch (const std::exception& ex) {
        Logger::error("main", std::string("Fatal: ") + ex.what());
        return EXIT_FAILURE;
    }

    std::cout << "\n\033[1;32m✔ Server shut down cleanly.\033[0m\n";
    return EXIT_SUCCESS;
}
