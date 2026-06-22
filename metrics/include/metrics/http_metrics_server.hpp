#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// http_metrics_server.hpp
//
// Minimal TCP HTTP server that serves the Prometheus /metrics endpoint.
// Runs on a dedicated background thread.
//
// Design:
//   • POSIX sockets only — no third-party HTTP library dependency.
//   • Accepts one connection at a time (inline handling in the accept loop).
//   • Always responds HTTP 200 with the Prometheus text payload regardless
//     of the request path — keeps implementation minimal.
//   • SO_REUSEADDR | SO_REUSEPORT allow fast restart without TIME_WAIT delay.
//   • stop() closes the listening socket to break accept() immediately.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <thread>

class HttpMetricsServer {
public:
    explicit HttpMetricsServer(uint16_t port = 7778);
    ~HttpMetricsServer();

    // Non-copyable.
    HttpMetricsServer(const HttpMetricsServer&)            = delete;
    HttpMetricsServer& operator=(const HttpMetricsServer&) = delete;

    // Bind and listen on the configured port, then launch the server thread.
    void start();

    // Signal the server loop to stop and join the thread.
    void stop();

private:
    void server_loop();

    uint16_t          port_;
    std::thread       server_thread_;
    std::atomic<bool> running_{false};
    int               listen_fd_{-1};
};
