// ─────────────────────────────────────────────────────────────────────────────
// benchmark_report.cpp
//
// Three output formats for BenchmarkReport:
//   to_table_string()  — Unicode box-drawing ASCII table
//   to_csv_string()    — RFC 4180 CSV
//   to_json_string()   — Compact JSON (hand-rolled, no library)
//   print()            — Writes table to stdout with a separator bar
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/benchmark_report.hpp>

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Internal formatting helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string d2s(double v, int decimals = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << v;
    return oss.str();
}

std::string i2s(int64_t v) {
    return std::to_string(v);
}

std::string u2s(uint64_t v) {
    return std::to_string(v);
}

// Pad or truncate `s` to exactly `width` chars (left-aligned).
std::string pad_left(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

std::string pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return std::string(width - s.size(), ' ') + s;
}

// Escape a string for JSON.
std::string json_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

// Escape a string for CSV (wrap in quotes, double internal quotes).
std::string csv_field(const std::string& s) {
    bool needs_quotes = (s.find(',') != std::string::npos ||
                         s.find('"') != std::string::npos ||
                         s.find('\n') != std::string::npos);
    if (!needs_quotes) return s;

    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    out += '"';
    return out;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// to_table_string
// ─────────────────────────────────────────────────────────────────────────────

std::string BenchmarkReport::to_table_string() const {
    // Box-drawing characters (UTF-8).
    // We use a simple 2-column table: Field | Value

    const size_t COL1 = 28;
    const size_t COL2 = 20;

    // Unicode borders
    const std::string TL = "┌", TR = "┐", BL = "└", BR = "┘";
    const std::string TM = "┬", BM = "┴", ML = "├", MR = "┤", MM = "┼";
    const std::string H  = "─", V  = "│";

    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r) {
        std::string h1, h2;
        for (size_t i = 0; i < COL1 + 2; ++i) h1 += H;
        for (size_t i = 0; i < COL2 + 2; ++i) h2 += H;
        return l + h1 + m + h2 + r + "\n";
    };

    auto row = [&](const std::string& f, const std::string& v) {
        return V + " " + pad_left(f, COL1) + " " +
               V + " " + pad_right(v, COL2) + " " + V + "\n";
    };

    std::string s;
    s += hline(TL, TM, TR);
    s += row("Field", "Value");
    s += hline(ML, MM, MR);
    s += row("Profile",           profile_name);
    s += row("Duration (actual)", d2s(static_cast<double>(duration_actual_ms) / 1000.0, 1) + "s");
    s += row("Jobs submitted",    u2s(jobs_submitted));
    s += row("Jobs completed",    u2s(jobs_completed));
    s += row("Jobs failed",       u2s(jobs_failed));
    s += row("Throughput (rps)",  d2s(throughput_rps, 2));
    s += row("Submit rate (rps)", d2s(submit_rps, 2));
    s += row("Queue depth (avg)", d2s(queue_depth_avg, 1));
    s += row("Queue depth (max)", i2s(queue_depth_max));
    s += row("E2E latency p50",   d2s(e2e_latency_p50_ms, 2) + " ms");
    s += row("E2E latency p99",   d2s(e2e_latency_p99_ms, 2) + " ms");
    s += row("Dispatch lat p50",  d2s(dispatch_latency_p50_ms, 2) + " ms");
    s += row("Dispatch lat p99",  d2s(dispatch_latency_p99_ms, 2) + " ms");
    s += row("Time-series points",u2s(static_cast<uint64_t>(series.size())));
    s += hline(BL, BM, BR);

    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// to_csv_string
// ─────────────────────────────────────────────────────────────────────────────

std::string BenchmarkReport::to_csv_string() const {
    std::ostringstream oss;

    // ── Summary header + row ─────────────────────────────────────────────────
    oss << "profile,duration_ms,jobs_submitted,jobs_completed,jobs_failed,"
           "throughput_rps,submit_rps,queue_depth_avg,queue_depth_max,"
           "e2e_p50_ms,e2e_p99_ms,dispatch_p50_ms,dispatch_p99_ms\n";

    oss << csv_field(profile_name) << ","
        << duration_actual_ms << ","
        << jobs_submitted << ","
        << jobs_completed << ","
        << jobs_failed << ","
        << d2s(throughput_rps) << ","
        << d2s(submit_rps) << ","
        << d2s(queue_depth_avg) << ","
        << queue_depth_max << ","
        << d2s(e2e_latency_p50_ms) << ","
        << d2s(e2e_latency_p99_ms) << ","
        << d2s(dispatch_latency_p50_ms) << ","
        << d2s(dispatch_latency_p99_ms) << "\n";

    // ── Time-series header + rows ─────────────────────────────────────────────
    if (!series.empty()) {
        oss << "\n";
        oss << "ts_ms,rps,queue_depth,e2e_p99_ms\n";
        for (const auto& pt : series) {
            oss << pt.ts_ms << ","
                << d2s(pt.rps) << ","
                << pt.queue_depth << ","
                << d2s(pt.e2e_p99_ms) << "\n";
        }
    }

    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// to_json_string
// ─────────────────────────────────────────────────────────────────────────────

std::string BenchmarkReport::to_json_string() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);

    oss << "{\n"
        << "  \"profile\": "           << json_str(profile_name) << ",\n"
        << "  \"duration_ms\": "       << duration_actual_ms << ",\n"
        << "  \"jobs_submitted\": "    << jobs_submitted << ",\n"
        << "  \"jobs_completed\": "    << jobs_completed << ",\n"
        << "  \"jobs_failed\": "       << jobs_failed << ",\n"
        << "  \"throughput_rps\": "    << d2s(throughput_rps, 6) << ",\n"
        << "  \"submit_rps\": "        << d2s(submit_rps, 6) << ",\n"
        << "  \"queue_depth_avg\": "   << d2s(queue_depth_avg, 6) << ",\n"
        << "  \"queue_depth_max\": "   << queue_depth_max << ",\n"
        << "  \"e2e_p50_ms\": "        << d2s(e2e_latency_p50_ms, 6) << ",\n"
        << "  \"e2e_p99_ms\": "        << d2s(e2e_latency_p99_ms, 6) << ",\n"
        << "  \"dispatch_p50_ms\": "   << d2s(dispatch_latency_p50_ms, 6) << ",\n"
        << "  \"dispatch_p99_ms\": "   << d2s(dispatch_latency_p99_ms, 6) << ",\n"
        << "  \"series\": [\n";

    for (size_t i = 0; i < series.size(); ++i) {
        const auto& pt = series[i];
        oss << "    {"
            << "\"ts_ms\": " << pt.ts_ms << ", "
            << "\"rps\": " << d2s(pt.rps, 4) << ", "
            << "\"queue_depth\": " << pt.queue_depth << ", "
            << "\"e2e_p99_ms\": " << d2s(pt.e2e_p99_ms, 4)
            << "}";
        if (i + 1 < series.size()) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n"
        << "}\n";

    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// print
// ─────────────────────────────────────────────────────────────────────────────

void BenchmarkReport::print() const {
    std::string sep;
    for (int i = 0; i < 54; ++i) sep += "═";
    std::cout << "\n" << sep << "\n";
    std::cout << "  BENCHMARK RESULTS\n";
    std::cout << sep << "\n";
    std::cout << to_table_string();
    std::cout << sep << "\n\n" << std::flush;
}
