#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// server.hpp — TCP Job Manager Server
//
// Architecture:
//   • A single EventLoop thread handles all I/O (accept, read, write).
//   • JobManager (Phase 1) is called from EventLoop callbacks — effectively
//     single-threaded with respect to the manager, so no extra locking needed.
//   • A separate "reaper" thread checks worker heartbeat timestamps every 30s
//     and evicts stale connections under a shared mutex.
//   • A "pending pulls" deque tracks workers waiting for a job. When a new job
//     arrives via SUBMIT_JOB, it is immediately dispatched to a pending worker.
//
// Thread model:
//   main thread → Server::run() → EventLoop::run() [blocks]
//   reaper_thread_ [background, terminates on stop()]
// ─────────────────────────────────────────────────────────────────────────────

#include <manager/job_manager.hpp>
#include <net/event_loop.hpp>
#include <net/tcp_connection.hpp>
#include <storage/i_storage_backend.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

class Server {
public:
    // Construct the server.  storage is passed to the internal JobManager.
    explicit Server(uint16_t port, std::unique_ptr<IStorageBackend> storage);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Start listening and run the event loop. Blocks until stop() is called.
    void run();

    // Signal the server to shut down gracefully. Thread-safe.
    void stop();

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    uint16_t    port_;
    int         listen_fd_{-1};
    JobManager  manager_;
    net::EventLoop loop_;

    // ── Connection tracking ───────────────────────────────────────────────────

    enum class ConnRole { UNKNOWN, CLIENT, WORKER };

    struct ConnState {
        std::unique_ptr<net::TcpConnection> conn;
        ConnRole                            role{ConnRole::UNKNOWN};
        std::string                         worker_id;  // set on REGISTER_WORKER
    };

    std::unordered_map<int, ConnState> conns_;  // fd → state (EventLoop thread only)

    // ── Pending-pull queue ────────────────────────────────────────────────────
    // Worker fds that sent PULL_JOB but found the queue empty.
    // When a new job arrives, we dispatch to the front of this deque.
    std::deque<int> pending_pulls_;

    // ── Dispatched-job tracking ───────────────────────────────────────────────
    // job_id → worker_fd; used to requeue jobs when a worker disconnects.
    std::unordered_map<std::string, int> dispatched_;  // EventLoop thread only

    // ── Heartbeat / reaper ────────────────────────────────────────────────────
    std::mutex                                     hb_mutex_;
    std::unordered_map<int, int64_t>               last_seen_ms_;  // fd → epoch ms
    std::set<int>                                  stale_fds_;     // written by reaper
    std::thread                                    reaper_thread_;
    static constexpr int64_t kHeartbeatTimeoutMs = 60'000;   // evict after 60s silence
    static constexpr int64_t kReaperIntervalMs   = 30'000;   // check every 30s

    // ── EventLoop callbacks ───────────────────────────────────────────────────

    void on_new_connection();
    void on_readable(int fd);
    void on_writable(int fd);

    void handle_message(int fd, ConnState& state, const net::Message& msg);
    void handle_submit_job     (int fd, const net::Message& msg);
    void handle_register_worker(int fd, ConnState& state, const net::Message& msg);
    void handle_pull_job       (int fd, ConnState& state);
    void handle_complete_job   (int fd, ConnState& state, const net::Message& msg);
    void handle_fail_job       (int fd, ConnState& state, const net::Message& msg);
    void handle_heartbeat      (int fd, const net::Message& msg);

    // Try to dispatch a job to a pending worker. Called after any job submission.
    void try_dispatch();

    // Send a message to a connection (enqueues + registers WRITE if needed).
    void send_to(int fd, const net::Message& msg);

    // Close connection and clean up dispatched jobs.
    void close_conn(int fd);

    // Reaper loop — runs on reaper_thread_.
    void reaper_loop();

    // Evict all fds marked stale by the reaper (called from EventLoop thread).
    void evict_stale();

    static int64_t now_ms();
};
