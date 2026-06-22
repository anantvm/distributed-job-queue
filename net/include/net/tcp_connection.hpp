#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// tcp_connection.hpp — Framed non-blocking TCP connection
//
// Wraps a non-blocking socket fd and provides:
//   • A read accumulation buffer that re-assembles length-prefixed frames.
//   • A write queue that handles partial sends (socket backpressure).
//   • A FIFO queue of fully-decoded Message objects ready for processing.
//
// All methods are called from the EventLoop thread (single-threaded access).
// ─────────────────────────────────────────────────────────────────────────────

#include <net/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>

namespace net {

class TcpConnection {
public:
    explicit TcpConnection(int fd);
    ~TcpConnection();

    // Non-copyable, non-movable (owns a raw fd).
    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    [[nodiscard]] int         fd()        const noexcept { return fd_; }
    [[nodiscard]] bool        is_closed() const noexcept { return closed_; }
    [[nodiscard]] bool        write_pending() const noexcept { return !write_buf_.empty(); }

    // ── I/O methods (call from EventLoop callbacks) ───────────────────────────

    // Read available bytes from fd. Parses complete frames into msg_queue_.
    // Returns false if the peer closed the connection (EOF) or on fatal error.
    [[nodiscard]] bool do_read();

    // Flush write_buf_ to fd. Returns true when the buffer is fully drained.
    [[nodiscard]] bool do_write();

    // ── Message queue ─────────────────────────────────────────────────────────

    [[nodiscard]] bool    has_message() const noexcept { return !msg_queue_.empty(); }
    [[nodiscard]] Message pop_message();

    // ── Sending ───────────────────────────────────────────────────────────────

    // Serialize msg and append to write_buf_.
    // Caller must ensure the fd becomes WRITE-registered in the EventLoop
    // if write_pending() is true after this call.
    void enqueue_send(const Message& msg);

    // Gracefully close the fd and mark as closed.
    void close_connection();

    // Human-readable label for logging.
    [[nodiscard]] std::string label() const;

private:
    int  fd_{-1};
    bool closed_{false};

    std::vector<uint8_t> read_buf_;   // raw accumulation buffer
    std::vector<uint8_t> write_buf_;  // serialized bytes awaiting send
    std::deque<Message> msg_queue_;   // fully parsed messages

    // Maximum sizes (guards against memory exhaustion from malformed clients).
    static constexpr size_t kReadChunk  = 4096;
    static constexpr size_t kMaxReadBuf = 64u * 1024 * 1024;  // 64 MiB
    static constexpr size_t kMaxPayload = 16u * 1024 * 1024;  // 16 MiB

    // Parse as many complete frames as possible from read_buf_.
    void parse_frames();
};

} // namespace net
