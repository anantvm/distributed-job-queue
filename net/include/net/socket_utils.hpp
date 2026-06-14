#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// socket_utils.hpp — POSIX TCP socket helpers
//
// Thin wrappers around socket(2)/bind(2)/listen(2)/connect(2)/accept(2).
// All functions throw std::runtime_error on unrecoverable errors.
// Non-blocking mode (O_NONBLOCK) is set on every returned fd.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>

namespace net {

// Set O_NONBLOCK on fd. Throws on failure.
void set_nonblocking(int fd);

// Set SO_REUSEADDR on fd (silent on failure — best-effort).
void set_reuseaddr(int fd);

// Disable Nagle algorithm on fd for lower per-message latency.
void set_tcp_nodelay(int fd);

// Create a non-blocking TCP listening socket on *port*.
// Returns the listening fd. Throws on failure.
[[nodiscard]] int tcp_listen(uint16_t port, int backlog = 128);

// Connect to host:port using a **blocking** connect().
// Returns the connected fd. Throws on failure.
// Used by clients and workers that connect once at startup.
[[nodiscard]] int tcp_connect_blocking(const std::string& host, uint16_t port);

// Accept one connection from a non-blocking listening socket.
// Returns the new fd, or -1 if no connection is pending (EAGAIN/EWOULDBLOCK).
// Throws on real errors.
[[nodiscard]] int tcp_accept(int listen_fd);

} // namespace net
