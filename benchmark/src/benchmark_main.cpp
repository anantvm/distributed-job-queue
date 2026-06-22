// ─────────────────────────────────────────────────────────────────────────────
// benchmark_main.cpp — CLI entry point for the distributed job-queue benchmark
//
// Usage:
//   benchmark_runner [options]
//
// Options:
//   --host         <hostname>    Server host (default: 127.0.0.1)
//   --port         <port>        Data port   (default: 7777)
//   --metrics-port <port>        Metrics HTTP port (default: 7778)
//   --profile      <name>        Workload profile:
//                                  baseline | sustained_load | burst_spike |
//                                  priority_storm | large_payload
//                                (default: baseline)
//   --output       <file.csv>    Write CSV report to this file (optional)
//   --json         <file.json>   Write JSON report to this file (optional)
//   --log-level    <level>       DEBUG|INFO|WARN|ERROR (default: INFO)
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/benchmark_runner.hpp>
#include <benchmark/workload_profile.hpp>
#include <common/logger.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// helpers
// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [OPTIONS]\n"
        << "\n"
        << "Options:\n"
        << "  --host         <host>   Server hostname      (default: 127.0.0.1)\n"
        << "  --port         <port>   Data port            (default: 7777)\n"
        << "  --metrics-port <port>   Metrics HTTP port    (default: 7778)\n"
        << "  --profile      <name>   Workload profile     (default: baseline)\n"
        << "                            baseline | sustained_load | burst_spike\n"
        << "                            priority_storm | large_payload\n"
        << "  --output       <file>   Write CSV to file    (optional)\n"
        << "  --json         <file>   Write JSON to file   (optional)\n"
        << "  --log-level    <lvl>    DEBUG|INFO|WARN|ERROR (default: INFO)\n"
        << "  --help                  Show this message\n"
        << "\n";
}

static WorkloadProfile select_profile(const std::string& name) {
    if (name == "sustained_load")  return WorkloadProfile::sustained_load();
    if (name == "burst_spike")     return WorkloadProfile::burst_spike();
    if (name == "priority_storm")  return WorkloadProfile::priority_storm();
    if (name == "large_payload")   return WorkloadProfile::large_payload();
    if (name == "baseline")        return WorkloadProfile::baseline();

    std::cerr << "Unknown profile: " << name
              << "\nValid profiles: baseline, sustained_load, burst_spike, "
                 "priority_storm, large_payload\n";
    std::exit(EXIT_FAILURE);
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "ERROR: Cannot open '" << path << "' for writing\n";
        return;
    }
    f << content;
    f.close();
    std::cout << "  Written: " << path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Defaults ──────────────────────────────────────────────────────────────
    std::string host          = "127.0.0.1";
    uint16_t    data_port     = 7777;
    uint16_t    metrics_port  = 7778;
    std::string profile_name  = "baseline";
    std::string output_csv;
    std::string output_json;
    std::string log_level_str = "INFO";

    // ── Parse arguments ───────────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto require_next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: " << flag << " requires an argument\n";
                std::exit(EXIT_FAILURE);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (arg == "--host") {
            host = require_next("--host");
        } else if (arg == "--port") {
            int v = std::atoi(require_next("--port"));
            if (v <= 0 || v > 65535) {
                std::cerr << "ERROR: Invalid --port value: " << v << "\n";
                return EXIT_FAILURE;
            }
            data_port = static_cast<uint16_t>(v);
        } else if (arg == "--metrics-port") {
            int v = std::atoi(require_next("--metrics-port"));
            if (v <= 0 || v > 65535) {
                std::cerr << "ERROR: Invalid --metrics-port value: " << v << "\n";
                return EXIT_FAILURE;
            }
            metrics_port = static_cast<uint16_t>(v);
        } else if (arg == "--profile") {
            profile_name = require_next("--profile");
        } else if (arg == "--output") {
            output_csv = require_next("--output");
        } else if (arg == "--json") {
            output_json = require_next("--json");
        } else if (arg == "--log-level") {
            log_level_str = require_next("--log-level");
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // ── Apply log level ───────────────────────────────────────────────────────
    if      (log_level_str == "DEBUG") Logger::set_level(LogLevel::DEBUG);
    else if (log_level_str == "INFO")  Logger::set_level(LogLevel::INFO);
    else if (log_level_str == "WARN")  Logger::set_level(LogLevel::WARN);
    else if (log_level_str == "ERROR") Logger::set_level(LogLevel::ERROR);
    else {
        std::cerr << "Unknown log level: " << log_level_str << "\n";
        return EXIT_FAILURE;
    }

    // ── Select profile ────────────────────────────────────────────────────────
    WorkloadProfile profile = select_profile(profile_name);

    Logger::info("Benchmark",
                 "Profile: " + profile.name +
                 "  host=" + host +
                 "  port=" + std::to_string(data_port) +
                 "  metrics_port=" + std::to_string(metrics_port));

    // ── Run benchmark ─────────────────────────────────────────────────────────
    BenchmarkRunner runner(profile, host, data_port, metrics_port);
    BenchmarkReport report = runner.run();

    // ── Print report to terminal ──────────────────────────────────────────────
    report.print();

    // ── Optionally write CSV ──────────────────────────────────────────────────
    if (!output_csv.empty()) {
        write_file(output_csv, report.to_csv_string());
    }

    // ── Optionally write JSON ─────────────────────────────────────────────────
    if (!output_json.empty()) {
        write_file(output_json, report.to_json_string());
    }

    Logger::info("Benchmark", "Done.");
    return EXIT_SUCCESS;
}
