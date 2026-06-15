#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// tcp_worker.hpp — Distributed Worker Process (Phase 2)
//
// Connects to the Job Manager Server via TCP, pulls jobs, executes them using
// the local HandlerRegistry, and reports results back.
//
// Protocol flow (per job):
//   Worker → PULL_JOB
//   Server → JOB_DISPATCH | NO_JOB | SERVER_SHUTDOWN
//   [on JOB_DISPATCH]  execute locally
//   Worker → COMPLETE_JOB | FAIL_JOB
//   Server → ACK
//
// Heartbeat: sent inline when NO_JOB is received (and every poll timeout),
// so the server always knows this worker is alive without a separate thread.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/stop_source.hpp>
#include <net/protocol.hpp>
#include <worker/handler_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

class TcpWorker {
public:
    // host, port, registry: required.
    // worker_id: optional — if empty, a UUID is generated. Pass a fixed ID to
    //   test reconnect scenarios where the same logical worker reconnects after crash.
    explicit TcpWorker(std::string host, uint16_t port,
                       const HandlerRegistry& registry,
                       std::string worker_id = "");
    ~TcpWorker();

    TcpWorker(const TcpWorker&)            = delete;
    TcpWorker& operator=(const TcpWorker&) = delete;

    // Block-run the pull loop. Returns when stop() is called or the server
    // sends SERVER_SHUTDOWN.
    void run();

    // Signal stop. Thread-safe (e.g., from a SIGINT handler).
    void stop();

    // Forcefully close the socket to simulate a crash.
    void disconnect();

    struct Stats {
        uint64_t jobs_executed{0};
        uint64_t jobs_succeeded{0};
        uint64_t jobs_failed{0};
    };

    [[nodiscard]] Stats      stats()     const;
    [[nodiscard]] std::string worker_id() const { return worker_id_; }

private:
    std::string            host_;
    uint16_t               port_;
    const HandlerRegistry& registry_;
    StopSource             stop_source_;
    std::string            worker_id_;
    int                    sock_fd_{-1};

    std::atomic<uint64_t> jobs_executed_{0};
    std::atomic<uint64_t> jobs_succeeded_{0};
    std::atomic<uint64_t> jobs_failed_{0};

    static constexpr int kPollTimeoutMs   = 5000;  // 5 s before sending heartbeat
    static constexpr int kReconnectSleep  = 2000;  // 2 s between reconnect attempts

    // Connect + register. Throws on failure.
    void connect_and_register();

    // Blocking send of a single Message.
    void send_msg(const net::Message& msg);

    // Blocking receive of a single Message.
    // Returns false if connection closed.
    [[nodiscard]] bool recv_msg(net::Message& out);

    [[nodiscard]] net::Message execute_job(const Job& job);
};
