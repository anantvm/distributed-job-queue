// ─────────────────────────────────────────────────────────────────────────────
// http_metrics_server.cpp
//
// POSIX TCP HTTP server implementing the Prometheus /metrics endpoint.
//
// Protocol:
//   Client sends: GET /metrics HTTP/1.1\r\n...
//   Server always responds: HTTP/1.1 200 OK + Prometheus text body.
//
// The server reads up to 1024 bytes of the HTTP request (sufficient for the
// request line and headers), then always responds with the full metrics body
// regardless of path — keeping implementation minimal and focused.
//
// Shutdown:
//   stop() atomically sets running_=false and closes listen_fd_.
//   The blocking accept() returns EBADF/EINVAL after the fd is closed,
//   which causes the loop to exit cleanly.
// ─────────────────────────────────────────────────────────────────────────────

#include <metrics/http_metrics_server.hpp>
#include <metrics/metrics_registry.hpp>
#include <common/logger.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

HttpMetricsServer::HttpMetricsServer(uint16_t port)
    : port_(port)
{}

HttpMetricsServer::~HttpMetricsServer() {
    stop();
}

// ─── start() ─────────────────────────────────────────────────────────────────

void HttpMetricsServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // Already running.
    }

    // Create TCP socket.
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Logger::error("HttpMetricsServer",
            std::string("socket() failed: ") + std::strerror(errno));
        running_.store(false);
        return;
    }

    // SO_REUSEADDR: allow rebind immediately after restart.
    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        Logger::warn("HttpMetricsServer",
            std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
    }

    // SO_REUSEPORT: multiple processes can bind same port (optional; best-effort).
#ifdef SO_REUSEPORT
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        Logger::warn("HttpMetricsServer",
            std::string("setsockopt(SO_REUSEPORT) failed: ") + std::strerror(errno));
    }
#endif

    // Bind.
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        Logger::error("HttpMetricsServer",
            std::string("bind() failed on port ") + std::to_string(port_)
            + ": " + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false);
        return;
    }

    // Listen with a reasonable backlog.
    if (::listen(listen_fd_, 16) < 0) {
        Logger::error("HttpMetricsServer",
            std::string("listen() failed: ") + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        running_.store(false);
        return;
    }

    Logger::info("HttpMetricsServer",
        "Listening on port " + std::to_string(port_) + " (GET /metrics)");

    server_thread_ = std::thread([this] { server_loop(); });
}

// ─── stop() ──────────────────────────────────────────────────────────────────

void HttpMetricsServer::stop() {
    running_.store(false, std::memory_order_relaxed);

    // Close the listening socket to break accept().
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

// ─── server_loop() ───────────────────────────────────────────────────────────

void HttpMetricsServer::server_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        struct sockaddr_in client_addr{};
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = ::accept(
            listen_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                // Interrupted by signal; retry.
                continue;
            }
            // Socket was closed by stop() or another error — exit loop.
            if (running_.load(std::memory_order_relaxed)) {
                Logger::warn("HttpMetricsServer",
                    std::string("accept() error: ") + std::strerror(errno));
            }
            break;
        }

        // ── Handle client inline ──────────────────────────────────────────────

        // Read the HTTP request (up to 1024 bytes).  We don't parse it beyond
        // acknowledging its existence — always return the metrics body.
        char request_buf[1024];
        ssize_t n = ::recv(client_fd, request_buf, sizeof(request_buf) - 1, 0);
        (void)n;  // We don't need to inspect the request content.

        // Build the Prometheus metrics body.
        const std::string body = MetricsRegistry::instance().prometheus_text();

        // Build HTTP response.
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "\r\n" + body;

        // Send full response (handle partial writes).
        const char* ptr   = response.data();
        std::size_t left  = response.size();
        while (left > 0) {
            ssize_t sent = ::send(client_fd, ptr, left, 0);
            if (sent <= 0) break;
            ptr  += sent;
            left -= static_cast<std::size_t>(sent);
        }

        ::close(client_fd);
    }

    Logger::info("HttpMetricsServer", "Server loop exited");
}
