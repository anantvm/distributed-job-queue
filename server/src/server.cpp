// server/src/server.cpp
#include <server/server.hpp>

#include <common/logger.hpp>
#include <net/json_util.hpp>
#include <net/protocol.hpp>
#include <net/socket_utils.hpp>


#include <chrono>
#include <thread>
#include <algorithm>
#include <unistd.h>


using namespace std::chrono_literals;
static constexpr const char* kComp = "Server";

// ─── Helpers ─────────────────────────────────────────────────────────────────

int64_t Server::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

Server::Server(uint16_t port, std::unique_ptr<IStorageBackend> storage)
    : port_(port), manager_(std::move(storage)) {
    if (auto r = manager_.initialize(); r.err()) {
        throw std::runtime_error("Server: manager init failed: " + r.error());
    }
    Logger::info(kComp, "Created on port " + std::to_string(port_));
}

Server::~Server() { stop(); }

// ─── run / stop ───────────────────────────────────────────────────────────────

void Server::run() {
    listen_fd_ = net::tcp_listen(port_);
    Logger::info(kComp, "Listening on port " + std::to_string(port_));

    loop_.add_fd(listen_fd_, net::Events::READ,
        [this](int fd, uint32_t) { (void)fd; on_new_connection(); });

    // Start reaper thread.
    reaper_thread_ = std::thread([this] { reaper_loop(); });

    loop_.run();  // blocks

    // Cleanup: broadcast shutdown to all clients/workers.
    auto shutdown_msg = net::make_server_shutdown();
    for (auto& [fd, state] : conns_) {
        state.conn->enqueue_send(shutdown_msg);
        static_cast<void>(state.conn->do_write());
        state.conn->close_connection();
    }
    conns_.clear();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }

    // Reaper thread exits because loop_.running() is false → loop_.stop() was called.
    if (reaper_thread_.joinable()) reaper_thread_.join();

    Logger::info(kComp, "Shutdown complete");
}

void Server::stop() {
    loop_.stop();
}

// ─── on_new_connection ────────────────────────────────────────────────────────

void Server::on_new_connection() {
    while (true) {
        int fd = net::tcp_accept(listen_fd_);
        if (fd < 0) break;  // EAGAIN — no more pending connections

        Logger::info(kComp, "Accepted connection fd=" + std::to_string(fd));

        ConnState state;
        state.conn = std::make_unique<net::TcpConnection>(fd);
        conns_.emplace(fd, std::move(state));

        loop_.add_fd(fd, net::Events::READ,
            [this, fd](int, uint32_t ev) {
                if (ev & net::Events::WRITE)   on_writable(fd);
                if (ev & (net::Events::READ | net::Events::HUP | net::Events::ERR))
                                                on_readable(fd);
            });

        // Track heartbeat timestamp for this new connection.
        {
            std::lock_guard<std::mutex> lk{hb_mutex_};
            last_seen_ms_[fd] = now_ms();
        }
    }
}

// ─── on_readable ─────────────────────────────────────────────────────────────

void Server::on_readable(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    ConnState& state = it->second;
    bool alive = state.conn->do_read();

    while (state.conn->has_message()) {
        handle_message(fd, state, state.conn->pop_message());
    }

    if (!alive || state.conn->is_closed()) {
        close_conn(fd);
    }

    // Check if reaper flagged any stale fds.
    evict_stale();
}

// ─── on_writable ─────────────────────────────────────────────────────────────

void Server::on_writable(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    bool flushed = it->second.conn->do_write();
    if (flushed) {
        // Nothing left to write — unwatch WRITE to avoid busy-polling.
        loop_.modify_fd(fd, net::Events::READ);
    }
}

// ─── handle_message ──────────────────────────────────────────────────────────

void Server::handle_message(int fd, ConnState& state, const net::Message& msg) {
    switch (msg.type) {
        case net::MsgType::SUBMIT_JOB:
            state.role = ConnRole::CLIENT;
            handle_submit_job(fd, msg);
            break;
        case net::MsgType::REGISTER_WORKER:
            handle_register_worker(fd, state, msg);
            break;
        case net::MsgType::PULL_JOB:
            handle_pull_job(fd, state);
            break;
        case net::MsgType::COMPLETE_JOB:
            handle_complete_job(fd, msg);
            break;
        case net::MsgType::FAIL_JOB:
            handle_fail_job(fd, msg);
            break;
        case net::MsgType::HEARTBEAT:
            handle_heartbeat(fd, msg);
            break;
        default:
            Logger::warn(kComp, "Unknown msg type from fd=" + std::to_string(fd));
            break;
    }
}

// ─── handle_submit_job ────────────────────────────────────────────────────────

void Server::handle_submit_job(int fd, const net::Message& msg) {
    const auto& j = msg.payload_json;
    std::string job_type    = json_util::get_string(j, "job_type");
    std::string payload     = json_util::get_string(j, "payload");
    Priority    priority    = priority_from_string(json_util::get_string(j, "priority"));
    int         max_retries = json_util::get_int   (j, "max_retries");

    auto result = manager_.submit_job(job_type, payload, priority, max_retries);
    if (result.ok()) {
        send_to(fd, net::make_job_submitted(result.value()));
        try_dispatch();  // wake any pending worker
    } else {
        send_to(fd, net::make_job_submitted_err(result.error()));
    }
}

// ─── handle_register_worker ───────────────────────────────────────────────────

void Server::handle_register_worker(int fd, ConnState& state,
                                    const net::Message& msg) {
    state.role      = ConnRole::WORKER;
    state.worker_id = net::parse_worker_id(msg);
    Logger::info(kComp, "Worker registered: " + state.worker_id
                         + " fd=" + std::to_string(fd));
    send_to(fd, net::make_register_ack(true));
}

// ─── handle_pull_job ─────────────────────────────────────────────────────────

void Server::handle_pull_job(int fd, ConnState& state) {
    auto job = manager_.try_pull_job();
    if (job) {
        dispatched_[job->job_id] = fd;
        Logger::info(kComp, "Dispatched job " + job->job_id
                             + " to worker " + state.worker_id);
        send_to(fd, net::make_job_dispatch(*job));
    } else {
        // No job — add to pending list; worker will wait.
        pending_pulls_.push_back(fd);
        send_to(fd, net::make_no_job());
    }
}

// ─── handle_complete_job ──────────────────────────────────────────────────────

void Server::handle_complete_job(int fd, const net::Message& msg) {
    std::string job_id = net::parse_job_id(msg);
    dispatched_.erase(job_id);
    auto r = manager_.complete_job(job_id);
    send_to(fd, net::make_ack(r.ok(), r.ok() ? "" : r.error()));
    try_dispatch();  // maybe another pending worker can be served
}

// ─── handle_fail_job ─────────────────────────────────────────────────────────

void Server::handle_fail_job(int fd, const net::Message& msg) {
    std::string job_id      = net::parse_job_id     (msg);
    std::string error       = net::parse_error      (msg);
    int         retry_count = net::parse_retry_count(msg);
    int         max_retries = net::parse_max_retries(msg);

    dispatched_.erase(job_id);
    auto r = manager_.fail_job(job_id, error, retry_count, max_retries);
    send_to(fd, net::make_ack(r.ok(), r.ok() ? "" : r.error()));
    try_dispatch();
}

// ─── handle_heartbeat ────────────────────────────────────────────────────────

void Server::handle_heartbeat(int fd, const net::Message& msg) {
    std::string wid = net::parse_worker_id(msg);
    Logger::debug(kComp, "Heartbeat from " + wid + " fd=" + std::to_string(fd));
    {
        std::lock_guard<std::mutex> lk{hb_mutex_};
        last_seen_ms_[fd] = now_ms();
    }
    send_to(fd, net::make_heartbeat_ack());
}

// ─── try_dispatch ─────────────────────────────────────────────────────────────
// Walk the pending-pull deque and dispatch a job to each waiting worker until
// either the queue is empty or all pending workers have been served.

void Server::try_dispatch() {
    while (!pending_pulls_.empty()) {
        int worker_fd = pending_pulls_.front();
        pending_pulls_.pop_front();

        // Check the worker is still connected.
        auto it = conns_.find(worker_fd);
        if (it == conns_.end() || it->second.conn->is_closed()) continue;

        auto job = manager_.try_pull_job();
        if (!job) break;  // in-memory queue is empty

        dispatched_[job->job_id] = worker_fd;
        Logger::info(kComp, "Dispatched (deferred) job " + job->job_id
                             + " to worker " + it->second.worker_id);
        send_to(worker_fd, net::make_job_dispatch(*job));
    }
}

// ─── send_to ─────────────────────────────────────────────────────────────────

void Server::send_to(int fd, const net::Message& msg) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    it->second.conn->enqueue_send(msg);
    // Try immediate flush; if not fully sent, watch for WRITE.
    bool done = it->second.conn->do_write();
    if (!done) {
        loop_.modify_fd(fd, net::Events::READ | net::Events::WRITE);
    }
}

// ─── close_conn ──────────────────────────────────────────────────────────────

void Server::close_conn(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    Logger::info(kComp, "Closing connection fd=" + std::to_string(fd)
                         + " worker=" + it->second.worker_id);

    // Requeue any jobs that were dispatched to this worker.
    std::vector<std::string> to_requeue;
    for (auto& [job_id, worker_fd] : dispatched_) {
        if (worker_fd == fd) to_requeue.push_back(job_id);
    }
    for (const auto& job_id : to_requeue) {
        dispatched_.erase(job_id);
        if (auto r = manager_.requeue_job(job_id); r.err()) {
            Logger::error(kComp, "requeue_job failed: " + r.error());
        }
    }

    // Remove from pending-pull deque.
    pending_pulls_.erase(
        std::remove(pending_pulls_.begin(), pending_pulls_.end(), fd),
        pending_pulls_.end());

    loop_.remove_fd(fd);
    it->second.conn->close_connection();
    conns_.erase(it);

    {
        std::lock_guard<std::mutex> lk{hb_mutex_};
        last_seen_ms_.erase(fd);
    }
}

// ─── reaper_loop ─────────────────────────────────────────────────────────────

void Server::reaper_loop() {
    int64_t elapsed_ms = 0;
    while (loop_.running()) {
        // Sleep in short increments so stop() wakes us up quickly.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        elapsed_ms += 500;
        if (!loop_.running()) break;
        if (elapsed_ms < kReaperIntervalMs) continue;
        elapsed_ms = 0;

        int64_t now = now_ms();
        std::lock_guard<std::mutex> lk{hb_mutex_};
        for (auto& [fd, ts] : last_seen_ms_) {
            if (now - ts > kHeartbeatTimeoutMs) {
                Logger::warn(kComp, "Evicting stale fd=" + std::to_string(fd)
                                     + " (silent for "
                                     + std::to_string((now - ts) / 1000) + "s)");
                stale_fds_.insert(fd);
            }
        }
    }
}

// ─── evict_stale ─────────────────────────────────────────────────────────────
// Called on the EventLoop thread to safely close stale connections.

void Server::evict_stale() {
    std::set<int> to_evict;
    {
        std::lock_guard<std::mutex> lk{hb_mutex_};
        to_evict.swap(stale_fds_);
    }
    for (int fd : to_evict) close_conn(fd);
}
