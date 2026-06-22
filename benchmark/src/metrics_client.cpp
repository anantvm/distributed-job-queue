// ─────────────────────────────────────────────────────────────────────────────
// metrics_client.cpp
//
// Raw TCP HTTP/1.0 GET + Prometheus text-format parser.
//
// Prometheus exposition format recap
// ────────────────────────────────────
// Lines starting with '#' are either TYPE hints or HELP lines.
// Data lines are:  metric_name [label_set] value [timestamp]
//
// We parse TYPE lines to know whether the next family is a counter, gauge, or
// histogram/summary.  For histogram _bucket lines we just store each bucket as
// a gauge named "<metric>{le=<X>}".  The _count and _sum suffixed lines are
// stored as regular gauges/counters so the caller can read them by name.
//
// Socket I/O uses POSIX APIs directly so there is no external dependency.
// ─────────────────────────────────────────────────────────────────────────────

#include <benchmark/metrics_client.hpp>

#include <common/logger.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std::chrono;

// ─────────────────────────────────────────────────────────────────────────────
// MetricsSnapshot helpers
// ─────────────────────────────────────────────────────────────────────────────

double MetricsSnapshot::get(const std::string& name) const noexcept {
    {
        auto it = gauges.find(name);
        if (it != gauges.end()) return it->second;
    }
    {
        auto it = counters.find(name);
        if (it != counters.end()) return it->second;
    }
    return 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// MetricsClient
// ─────────────────────────────────────────────────────────────────────────────

MetricsClient::MetricsClient(std::string host, uint16_t metrics_port)
    : host_(std::move(host))
    , port_(metrics_port) {}

// ── http_get ──────────────────────────────────────────────────────────────────

std::string MetricsClient::http_get() {
    // ── Resolve hostname ──────────────────────────────────────────────────────
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICSERV;

    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port_);

    int rc = ::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        Logger::warn("MetricsClient",
                     "getaddrinfo failed: " + std::string(gai_strerror(rc)));
        return {};
    }

    // ── Connect ───────────────────────────────────────────────────────────────
    int fd = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;

        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
        Logger::warn("MetricsClient",
                     "connect to " + host_ + ":" + port_str + " failed: " +
                     std::string(::strerror(errno)));
        return {};
    }

    // Set a 5-second receive timeout so we don't hang forever.
    struct timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));

    // ── Send HTTP/1.0 request ─────────────────────────────────────────────────
    const std::string request =
        "GET /metrics HTTP/1.0\r\n"
        "Host: " + host_ + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    const char* ptr = request.data();
    size_t      rem = request.size();
    while (rem > 0) {
        ssize_t sent = ::send(fd, ptr, rem, 0);
        if (sent <= 0) {
            ::close(fd);
            return {};
        }
        ptr += sent;
        rem -= static_cast<size_t>(sent);
    }

    // ── Read response ─────────────────────────────────────────────────────────
    std::string response;
    response.reserve(8192);

    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // ── Strip HTTP headers (find \r\n\r\n) ────────────────────────────────────
    const std::string sep = "\r\n\r\n";
    auto pos = response.find(sep);
    if (pos == std::string::npos) {
        // Fallback: try \n\n
        const std::string sep2 = "\n\n";
        pos = response.find(sep2);
        if (pos == std::string::npos) return response; // return whatever we got
        return response.substr(pos + sep2.size());
    }
    return response.substr(pos + sep.size());
}

// ── parse ─────────────────────────────────────────────────────────────────────

namespace {

/// Trim leading and trailing whitespace from a string_view.
std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\t' ||
                            sv.back()  == '\r'|| sv.back()  == '\n')) sv.remove_suffix(1);
    return sv;
}

/// Parse a double from a string_view; returns 0.0 on failure.
double parse_double(std::string_view sv) {
    // Use strtod for correctness; need a null-terminated string.
    std::string s(sv);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return 0.0;
    return v;
}

/// Determine if a metric family name ends with "_total" (counter convention).
bool looks_like_counter(const std::string& name) {
    return name.size() > 6 &&
           name.substr(name.size() - 6) == "_total";
}

} // anonymous namespace

MetricsSnapshot MetricsClient::parse(const std::string& body,
                                     int64_t            timestamp_ms) {
    MetricsSnapshot snap;
    snap.timestamp_ms = timestamp_ms;

    // Current TYPE for the metric family being processed.
    // Values: "counter", "gauge", "histogram", "summary", "untyped"
    std::string current_type;
    std::string current_family;

    std::istringstream stream(body);
    std::string line;

    while (std::getline(stream, line)) {
        std::string_view sv = trim(line);
        if (sv.empty()) continue;

        // ── Comment / TYPE / HELP ─────────────────────────────────────────────
        if (sv[0] == '#') {
            // E.g.: "# TYPE job_queue_depth gauge"
            //       "# HELP job_queue_depth ..."
            std::istringstream comment{std::string(sv)};
            std::string hash, keyword, family, type_word;
            comment >> hash >> keyword;
            if (keyword == "TYPE") {
                comment >> family >> type_word;
                current_family = family;
                current_type   = type_word;
            }
            continue;
        }

        // ── Data line ─────────────────────────────────────────────────────────
        // Format: metric_name[{labels}] value [timestamp]
        // We split on the last space to get value, then everything before is
        // the full metric identifier (name + optional labels).

        // Find the label block if present.
        std::string_view identifier;
        std::string_view value_sv;

        // Look for the last space that separates value from name+labels.
        // Prometheus format: no space inside labels' string values is common but
        // we handle it by finding '{' first.
        auto brace_open = sv.find('{');
        if (brace_open != std::string_view::npos) {
            auto brace_close = sv.find('}', brace_open);
            if (brace_close == std::string_view::npos) {
                // Malformed; skip
                continue;
            }
            // After the closing brace there may be a space then optional timestamp.
            auto after_brace = sv.find(' ', brace_close);
            if (after_brace == std::string_view::npos) continue;

            identifier = sv.substr(0, after_brace);
            // Skip optional second space (timestamp field); value is right after first space.
            std::string_view rest = trim(sv.substr(after_brace + 1));
            // The value might be followed by a timestamp; take the first token.
            auto sp2 = rest.find(' ');
            value_sv = (sp2 != std::string_view::npos) ? rest.substr(0, sp2) : rest;
        } else {
            auto sp = sv.rfind(' ');
            if (sp == std::string_view::npos) continue;
            identifier = sv.substr(0, sp);
            std::string_view rest = trim(sv.substr(sp + 1));
            auto sp2 = rest.find(' ');
            value_sv = (sp2 != std::string_view::npos) ? rest.substr(0, sp2) : rest;
        }

        identifier = trim(identifier);
        value_sv   = trim(value_sv);

        if (identifier.empty() || value_sv.empty()) continue;

        // Skip NaN and +Inf / -Inf values.
        if (value_sv == "NaN" || value_sv == "+Inf" || value_sv == "-Inf") continue;

        double value = parse_double(value_sv);

        // ── Bucket lines: store as gauge with encoded name ────────────────────
        // e.g. job_latency_ms_bucket{le="99.0"} 1234
        std::string name(identifier);

        // Determine storage bucket based on TYPE hint.
        bool is_counter = (current_type == "counter") || looks_like_counter(name);

        if (is_counter) {
            snap.counters[name] = value;
        } else {
            snap.gauges[name] = value;
        }
    }

    return snap;
}

// ── poll ──────────────────────────────────────────────────────────────────────

MetricsSnapshot MetricsClient::poll() {
    int64_t ts = duration_cast<milliseconds>(
                     system_clock::now().time_since_epoch())
                     .count();

    std::string body = http_get();
    if (body.empty()) {
        MetricsSnapshot empty;
        empty.timestamp_ms = ts;
        return empty;
    }
    return parse(body, ts);
}

// ── is_reachable ──────────────────────────────────────────────────────────────

bool MetricsClient::is_reachable() {
    return !http_get().empty();
}
