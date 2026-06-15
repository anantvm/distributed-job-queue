// worker_process/src/tcp_worker.cpp
#include <worker_process/tcp_worker.hpp>

#include <common/logger.hpp>
#include <common/uuid.hpp>
#include <net/socket_utils.hpp>

#include <arpa/inet.h>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static constexpr const char* kComp = "TcpWorker";

// ─── Constructor / Destructor ─────────────────────────────────────────────────

TcpWorker::TcpWorker(std::string host, uint16_t port,
                     const HandlerRegistry& registry,
                     std::string worker_id)
    : host_(std::move(host))
    , port_(port)
    , registry_(registry)
    , worker_id_(worker_id.empty() ? uuid::generate() : std::move(worker_id)) {
    Logger::info(kComp, "Worker created id=" + worker_id_);
}

TcpWorker::~TcpWorker() { disconnect(); }

void TcpWorker::stop()    { stop_source_.request_stop(); }
void TcpWorker::disconnect() {
    if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }
}

// ─── send_msg / recv_msg ─────────────────────────────────────────────────────

void TcpWorker::send_msg(const net::Message& msg) {
    auto bytes = net::serialize(msg);
    const uint8_t* ptr = bytes.data();
    size_t rem = bytes.size();
    while (rem > 0) {
        ssize_t n = ::send(sock_fd_, ptr, rem, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            throw std::runtime_error("send_msg: " + std::string(std::strerror(errno)));
        }
        ptr += n;
        rem -= static_cast<size_t>(n);
    }
}

bool TcpWorker::recv_msg(net::Message& out) {
    // Poll with timeout so the stop flag can be checked.
    pollfd pfd{};
    pfd.fd     = sock_fd_;
    pfd.events = POLLIN;

    int rc = ::poll(&pfd, 1, kPollTimeoutMs);
    if (rc == 0) return false;  // timeout — caller sends heartbeat
    if (rc < 0) {
        if (errno == EINTR) return false;
        throw std::runtime_error("poll: " + std::string(std::strerror(errno)));
    }
    if (pfd.revents & (POLLHUP | POLLERR)) {
        throw std::runtime_error("connection closed by server");
    }

    // Read the 8-byte header.
    uint8_t hdr[8];
    size_t  got = 0;
    while (got < 8) {
        ssize_t n = ::recv(sock_fd_, hdr + got, 8 - got, 0);
        if (n == 0) throw std::runtime_error("server closed connection");
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("recv header: " + std::string(std::strerror(errno)));
        }
        got += static_cast<size_t>(n);
    }

    uint32_t type_be{}, plen_be{};
    std::memcpy(&type_be, hdr,     4);
    std::memcpy(&plen_be, hdr + 4, 4);
    uint32_t plen = ntohl(plen_be);

    // Read payload.
    std::vector<uint8_t> payload(plen);
    got = 0;
    while (got < plen) {
        ssize_t n = ::recv(sock_fd_, payload.data() + got, plen - got, 0);
        if (n == 0) throw std::runtime_error("server closed connection mid-payload");
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("recv payload: " + std::string(std::strerror(errno)));
        }
        got += static_cast<size_t>(n);
    }

    out.type         = static_cast<net::MsgType>(ntohl(type_be));
    out.payload_json = std::string(reinterpret_cast<char*>(payload.data()), plen);
    return true;
}

// ─── connect_and_register ─────────────────────────────────────────────────────

void TcpWorker::connect_and_register() {
    // Ignore SIGPIPE so send() returns EPIPE on a closed socket rather than
    // terminating the process. Standard practice for network clients.
    signal(SIGPIPE, SIG_IGN);

    sock_fd_ = net::tcp_connect_blocking(host_, port_);
    Logger::info(kComp, "Connected to " + host_ + ":" + std::to_string(port_));

    send_msg(net::make_register_worker(worker_id_));

    net::Message ack;
    if (!recv_msg(ack) || ack.type != net::MsgType::REGISTER_ACK) {
        disconnect();
        throw std::runtime_error("Did not receive REGISTER_ACK");
    }
    Logger::info(kComp, "Registered with server");
}

// ─── execute_job ─────────────────────────────────────────────────────────────

net::Message TcpWorker::execute_job(const Job& job) {
    Logger::info(kComp,
        "Executing job=" + job.job_id + " type=" + job.job_type
        + " attempt=" + std::to_string(job.retry_count + 1));

    ++jobs_executed_;

    auto handler_opt = registry_.find(job.job_type);
    if (!handler_opt) {
        ++jobs_failed_;
        return net::make_fail_job(job.job_id,
                                  "No handler for type: " + job.job_type,
                                  job.max_retries, job.max_retries);  // permanent fail
    }

    try {
        (*handler_opt)(job);
        ++jobs_succeeded_;
        Logger::info(kComp, "Completed job=" + job.job_id);
        return net::make_complete_job(job.job_id);
    } catch (const std::exception& ex) {
        ++jobs_failed_;
        Logger::warn(kComp, "Job " + job.job_id + " threw: " + ex.what());
        return net::make_fail_job(job.job_id, ex.what(),
                                  job.retry_count, job.max_retries);
    } catch (...) {
        ++jobs_failed_;
        return net::make_fail_job(job.job_id, "unknown exception",
                                  job.retry_count, job.max_retries);
    }
}

// ─── run — main pull loop ─────────────────────────────────────────────────────

void TcpWorker::run() {
    const StopToken stop_tok = stop_source_.get_token();

    while (!stop_tok.stop_requested()) {
        // (Re-)connect if needed.
        if (sock_fd_ < 0) {
            try {
                connect_and_register();
            } catch (const std::exception& ex) {
                Logger::error(kComp, "Connect failed: " + std::string(ex.what())
                                      + " — retrying in " + std::to_string(kReconnectSleep) + "ms");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kReconnectSleep));
                continue;
            }
        }

        try {
            // Send PULL_JOB.
            send_msg(net::make_pull_job(worker_id_));

            // Wait for response (with poll timeout).
            net::Message resp;
            bool got = recv_msg(resp);

            if (!got) {
                // Timeout — send heartbeat to keep the connection alive.
                send_msg(net::make_heartbeat(worker_id_));
                // Don't wait for HEARTBEAT_ACK — just continue the loop.
                continue;
            }

            if (resp.type == net::MsgType::SERVER_SHUTDOWN) {
                Logger::info(kComp, "Server sent shutdown — exiting");
                break;
            }

            if (resp.type == net::MsgType::NO_JOB) {
                // Queue is empty — send heartbeat and back off briefly.
                send_msg(net::make_heartbeat(worker_id_));
                std::this_thread::sleep_for(100ms);
                continue;
            }

            if (resp.type == net::MsgType::JOB_DISPATCH) {
                Job job = net::parse_job(resp);
                net::Message outcome = execute_job(job);
                send_msg(outcome);

                // Read ACK (required to advance protocol state).
                net::Message ack;
                static_cast<void>(recv_msg(ack));  // best-effort; ignore if server closed
                continue;
            }

            if (resp.type == net::MsgType::HEARTBEAT_ACK) {
                continue;  // received stale heartbeat ack — ignore
            }

            Logger::warn(kComp, "Unexpected message type in pull loop");

        } catch (const std::exception& ex) {
            Logger::error(kComp, "Connection error: " + std::string(ex.what())
                                   + " — reconnecting");
            disconnect();
        }
    }

    disconnect();
    Logger::info(kComp, "Worker loop exited");
}

// ─── stats ────────────────────────────────────────────────────────────────────

TcpWorker::Stats TcpWorker::stats() const {
    return {
        .jobs_executed  = jobs_executed_ .load(std::memory_order_relaxed),
        .jobs_succeeded = jobs_succeeded_.load(std::memory_order_relaxed),
        .jobs_failed    = jobs_failed_   .load(std::memory_order_relaxed),
    };
}
