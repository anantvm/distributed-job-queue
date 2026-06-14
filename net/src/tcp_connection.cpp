// net/src/tcp_connection.cpp
#include <net/tcp_connection.hpp>

#include <common/logger.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace net {

TcpConnection::TcpConnection(int fd) : fd_(fd) {}

TcpConnection::~TcpConnection() {
    close_connection();
}

// ─── do_read ─────────────────────────────────────────────────────────────────
// Called each time the EventLoop fires a READ event on this fd.
// Reads all available bytes (loops until EAGAIN / EWOULDBLOCK), then
// calls parse_frames() to extract complete messages.

bool TcpConnection::do_read() {
    if (closed_) return false;

    while (true) {
        if (read_buf_.size() >= kMaxReadBuf) {
            Logger::error("TcpConn", label() + " read buffer overflow — closing");
            close_connection();
            return false;
        }

        uint8_t tmp[kReadChunk];
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);

        if (n > 0) {
            read_buf_.insert(read_buf_.end(), tmp, tmp + n);
        } else if (n == 0) {
            // EOF — peer closed connection
            closed_ = true;
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
            if (errno == EINTR) continue;
            Logger::error("TcpConn", label() + " recv error: " + std::strerror(errno));
            close_connection();
            return false;
        }
    }

    parse_frames();
    return true;
}

// ─── parse_frames ─────────────────────────────────────────────────────────────
// Extract as many complete messages as possible from read_buf_.

void TcpConnection::parse_frames() {
    size_t offset = 0;
    while (true) {
        Message msg;
        size_t  consumed = 0;
        if (!try_deserialize(read_buf_.data() + offset,
                             read_buf_.size() - offset,
                             msg, consumed)) {
            break;  // not enough data for a complete frame yet
        }
        msg_queue_.push_back(std::move(msg));
        offset += consumed;
    }

    if (offset > 0) {
        // Remove processed bytes from the front of the buffer.
        read_buf_.erase(read_buf_.begin(),
                        read_buf_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

// ─── pop_message ──────────────────────────────────────────────────────────────

Message TcpConnection::pop_message() {
    if (msg_queue_.empty())
        throw std::logic_error("TcpConnection::pop_message() on empty queue");
    Message m = std::move(msg_queue_.front());
    msg_queue_.erase(msg_queue_.begin());
    return m;
}

// ─── enqueue_send ─────────────────────────────────────────────────────────────
// Serialise msg and append to write_buf_. Caller must monitor fd for WRITE.

void TcpConnection::enqueue_send(const Message& msg) {
    if (closed_) return;
    auto bytes = serialize(msg);
    write_buf_.insert(write_buf_.end(), bytes.begin(), bytes.end());
}

// ─── do_write ────────────────────────────────────────────────────────────────
// Flush as much of write_buf_ as the socket will accept right now.
// Returns true when the buffer is empty (fully flushed).

bool TcpConnection::do_write() {
    if (closed_ || write_buf_.empty()) return true;

    while (!write_buf_.empty()) {
        ssize_t n = ::send(fd_, write_buf_.data(), write_buf_.size(), 0);
        if (n > 0) {
            write_buf_.erase(write_buf_.begin(),
                             write_buf_.begin() + n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // try again later
            if (errno == EINTR) continue;
            Logger::error("TcpConn", label() + " send error: " + std::strerror(errno));
            close_connection();
            return true;  // treat as drained to avoid stale WRITE watches
        }
    }
    return write_buf_.empty();
}

// ─── close_connection ─────────────────────────────────────────────────────────

void TcpConnection::close_connection() {
    if (!closed_ && fd_ >= 0) {
        ::close(fd_);
        closed_ = true;
    }
}

std::string TcpConnection::label() const {
    return "fd=" + std::to_string(fd_);
}

} // namespace net
