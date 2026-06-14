// tests/integration/test_net_integration.cpp
//
// Integration tests: spin up a real Server (in a background thread),
// connect with JobClient, and verify end-to-end behavior.
// A TcpWorker is also exercised in the full-flow test.
//
// Uses port 17777 to avoid clashing with the production server (7777).
// Tests run sequentially; the server thread is joined between each suite.

#include <server/server.hpp>
#include <client/job_client.hpp>
#include <worker_process/tcp_worker.hpp>
#include <storage/in_memory_backend.hpp>
#include <worker/handler_registry.hpp>
#include <common/logger.hpp>
#include <net/protocol.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <csignal>

using namespace std::chrono_literals;

// Ignore SIGPIPE globally: when the server shuts down and a worker tries to
// write to the now-closed socket, the kernel sends SIGPIPE. Without SIG_IGN
// the process dies. send(2) returns -1/EPIPE instead, which we already handle.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} _sigpipe_ignorer;

// Port base for integration tests.
// Each test uses a different port (kBasePort + N) to avoid TIME_WAIT collisions
// when CTest runs each test as a separate process in rapid succession.
static constexpr uint16_t kBasePort = 17770;

static uint16_t test_port(uint16_t offset) {
    return static_cast<uint16_t>(kBasePort + offset);
}

static std::unique_ptr<Server> make_server(uint16_t port) {
    return std::make_unique<Server>(port, std::make_unique<InMemoryBackend>());
}

static std::pair<std::unique_ptr<Server>, std::thread>
start_server(uint16_t port) {
    auto srv = make_server(port);
    auto* raw = srv.get();
    std::thread t([raw] { raw->run(); });
    std::this_thread::sleep_for(80ms);  // give the kernel time to bind
    return {std::move(srv), std::move(t)};
}

static void stop_server(std::unique_ptr<Server>& srv, std::thread& t) {
    srv->stop();
    if (t.joinable()) t.join();
}

// ─── Test: client submits a job ───────────────────────────────────────────────

TEST(NetIntegration, ClientSubmitReceivesJobId) {
    auto [srv, t] = start_server(test_port(0));

    JobClient client("127.0.0.1", test_port(0));
    auto r = client.submit("sleep_job", "10", Priority::NORMAL, 1);

    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error());
    EXPECT_FALSE(r.value().empty());

    stop_server(srv, t);
}

TEST(NetIntegration, MultipleClientSubmits) {
    auto [srv, t] = start_server(test_port(1));

    std::vector<std::string> ids;
    JobClient client("127.0.0.1", test_port(1));
    for (int i = 0; i < 5; ++i) {
        auto r = client.submit("sleep_job", "1", Priority::NORMAL, 1);
        ASSERT_TRUE(r.ok()) << r.error();
        ids.push_back(r.value());
    }

    // All job IDs should be unique.
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());

    stop_server(srv, t);
}

TEST(NetIntegration, PriorityPreservedInSubmit) {
    auto [srv, t] = start_server(test_port(2));

    JobClient client("127.0.0.1", test_port(2));
    auto r_hi  = client.submit("sleep_job", "1", Priority::HIGH,   1);
    auto r_lo  = client.submit("sleep_job", "1", Priority::LOW,    1);

    EXPECT_TRUE(r_hi.ok()) << r_hi.error();
    EXPECT_TRUE(r_lo.ok()) << r_lo.error();
    EXPECT_NE(r_hi.value(), r_lo.value());

    stop_server(srv, t);
}

// ─── Test: worker connects and executes a job ─────────────────────────────────

TEST(NetIntegration, WorkerPullsAndCompletesJob) {
    Logger::set_level(LogLevel::WARN);  // less noise in tests

    auto [srv, t] = start_server(test_port(3));

    // Register a fast handler.
    HandlerRegistry registry;
    std::atomic<bool> executed{false};
    registry.register_handler("test_job", [&executed](const Job&) {
        executed = true;
    });

    // Submit a job.
    JobClient client("127.0.0.1", test_port(3));
    auto submit_r = client.submit("test_job", "", Priority::NORMAL, 1);
    ASSERT_TRUE(submit_r.ok()) << submit_r.error();

    // Start a worker; it should pull and execute the job.
    TcpWorker worker("127.0.0.1", test_port(3), registry);
    std::thread worker_thread([&worker] { worker.run(); });

    // Wait up to 2 seconds for execution.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!executed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(executed.load());

    worker.stop();
    if (worker_thread.joinable()) worker_thread.join();
    stop_server(srv, t);

    EXPECT_EQ(worker.stats().jobs_executed, 1u);
    EXPECT_EQ(worker.stats().jobs_succeeded, 1u);

    Logger::set_level(LogLevel::INFO);
}

TEST(NetIntegration, FailedJobIsRetried) {
    Logger::set_level(LogLevel::WARN);
    auto [srv, t] = start_server(test_port(4));

    HandlerRegistry registry;
    std::atomic<int> attempts{0};
    registry.register_handler("flaky", [&attempts](const Job& job) {
        ++attempts;
        if (job.retry_count == 0) throw std::runtime_error("first attempt fails");
    });

    JobClient client("127.0.0.1", test_port(4));
    auto r = client.submit("flaky", "", Priority::NORMAL, /*max_retries=*/2);
    ASSERT_TRUE(r.ok()) << r.error();

    TcpWorker worker("127.0.0.1", test_port(4), registry);
    std::thread wt([&worker] { worker.run(); });

    // Job fails once, retries, then succeeds → 2 attempts.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (attempts.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
    }

    worker.stop();
    if (wt.joinable()) wt.join();
    stop_server(srv, t);

    EXPECT_GE(attempts.load(), 2);

    Logger::set_level(LogLevel::INFO);
}

TEST(NetIntegration, MultipleWorkersShareJobs) {
    Logger::set_level(LogLevel::WARN);
    auto [srv, t] = start_server(test_port(5));

    HandlerRegistry registry;
    std::atomic<int> done{0};
    registry.register_handler("count_job", [&done](const Job&) { ++done; });

    // Submit 10 jobs.
    JobClient client("127.0.0.1", test_port(5));
    for (int i = 0; i < 10; ++i) {
        auto r = client.submit("count_job", "", Priority::NORMAL, 1);
        ASSERT_TRUE(r.ok()) << r.error();
    }

    // Start 3 workers.
    std::vector<std::unique_ptr<TcpWorker>> workers;
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        workers.push_back(std::make_unique<TcpWorker>("127.0.0.1", test_port(5), registry));
        auto* w = workers.back().get();
        threads.emplace_back([w] { w->run(); });
    }

    // Wait up to 5 s for all 10 jobs to be executed.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (done.load() < 10 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(done.load(), 10);

    for (auto& w : workers) w->stop();
    for (auto& thr : threads) if (thr.joinable()) thr.join();
    stop_server(srv, t);

    Logger::set_level(LogLevel::INFO);
}
