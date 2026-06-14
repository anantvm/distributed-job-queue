// client_lib/src/job_client.cpp
#include <client/job_client.hpp>

#include <common/logger.hpp>
#include <net/protocol.hpp>
#include <net/socket_utils.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

JobClient::JobClient(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

JobClient::~JobClient() { disconnect(); }

// ─── connect / disconnect ─────────────────────────────────────────────────────

VoidResult JobClient::connect() {
    if (fd_ >= 0) return VoidResult::Ok();  // already connected
    try {
        fd_ = net::tcp_connect_blocking(host_, port_);
        Logger::debug("JobClient", "Connected to " + host_ + ":" + std::to_string(port_));
        return VoidResult::Ok();
    } catch (const std::exception& ex) {
        return VoidResult::Err(std::string("connect: ") + ex.what());
    }
}

void JobClient::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

// ─── send_all / recv_all ─────────────────────────────────────────────────────

VoidResult JobClient::send_all(const void* data, size_t len) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd_, ptr, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            disconnect();
            return VoidResult::Err("send: " + std::string(std::strerror(errno)));
        }
        ptr += n;
        len -= static_cast<size_t>(n);
    }
    return VoidResult::Ok();
}

VoidResult JobClient::recv_all(void* data, size_t len) {
    auto* ptr = static_cast<uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd_, ptr, len, 0);
        if (n == 0) { disconnect(); return VoidResult::Err("connection closed"); }
        if (n < 0) {
            if (errno == EINTR) continue;
            disconnect();
            return VoidResult::Err("recv: " + std::string(std::strerror(errno)));
        }
        ptr += n;
        len -= static_cast<size_t>(n);
    }
    return VoidResult::Ok();
}

// ─── transact ────────────────────────────────────────────────────────────────
// Send one Message and receive one Message using the length-prefix framing.

Result<net::Message> JobClient::transact(const net::Message& request) {
    auto bytes = net::serialize(request);
    if (auto r = send_all(bytes.data(), bytes.size()); r.err())
        return Result<net::Message>::Err(r.error());

    // Read header (8 bytes).
    uint8_t hdr[8];
    if (auto r = recv_all(hdr, 8); r.err())
        return Result<net::Message>::Err(r.error());

    uint32_t type_be{}, plen_be{};
    std::memcpy(&type_be, hdr,     4);
    std::memcpy(&plen_be, hdr + 4, 4);
    uint32_t plen = ntohl(plen_be);

    // Read payload.
    std::vector<uint8_t> payload(plen);
    if (plen > 0) {
        if (auto r = recv_all(payload.data(), plen); r.err())
            return Result<net::Message>::Err(r.error());
    }

    net::Message resp;
    resp.type         = static_cast<net::MsgType>(ntohl(type_be));
    resp.payload_json = std::string(reinterpret_cast<char*>(payload.data()), plen);
    return Result<net::Message>::Ok(std::move(resp));
}

// ─── submit ──────────────────────────────────────────────────────────────────

Result<std::string> JobClient::submit(const std::string& job_type,
                                      const std::string& payload,
                                      Priority           priority,
                                      int                max_retries) {
    // Reconnect if needed.
    if (fd_ < 0) {
        if (auto r = connect(); r.err())
            return Result<std::string>::Err(r.error());
    }

    auto req = net::make_submit_job(job_type, payload, priority, max_retries);
    auto resp_r = transact(req);
    if (resp_r.err())
        return Result<std::string>::Err(resp_r.error());

    const auto& resp = resp_r.value();
    if (resp.type != net::MsgType::JOB_SUBMITTED)
        return Result<std::string>::Err("unexpected response type");

    if (!net::parse_ok(resp))
        return Result<std::string>::Err(net::parse_error(resp));

    // Each submission uses a fresh connection (one-shot model: connect → submit → close).
    // This avoids persistent connection state on the client side.
    disconnect();

    return Result<std::string>::Ok(net::parse_job_id(resp));
}
