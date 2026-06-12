// tests/unit/test_worker.cpp
//
// Integration tests for Worker + JobManager + HandlerRegistry together.
// Uses InMemoryBackend for speed; focuses on end-to-end job execution paths.

#include <manager/job_manager.hpp>
#include <storage/in_memory_backend.hpp>
#include <worker/handler_registry.hpp>
#include <worker/worker.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Fixture ──────────────────────────────────────────────────────────────────

class WorkerTest : public ::testing::Test {
protected:
    std::unique_ptr<JobManager>  manager;
    HandlerRegistry              registry;
    std::atomic<int>             handler_calls{0};

    void SetUp() override {
        manager = std::make_unique<JobManager>(
            std::make_unique<InMemoryBackend>());
        ASSERT_TRUE(manager->initialize().ok());
    }

    void TearDown() override {
        manager->shutdown();
    }

    // Wait until a condition becomes true, with a timeout.
    static bool wait_until(std::function<bool()> pred,
                            std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
};

// ─── Handler registry ─────────────────────────────────────────────────────────

TEST_F(WorkerTest, HandlerRegistryRegistersAndFinds) {
    registry.register_handler("my_type", [](const Job&) {});
    EXPECT_TRUE(registry.has_handler("my_type"));
    EXPECT_FALSE(registry.has_handler("other_type"));
    EXPECT_EQ(registry.size(), 1u);
}

TEST_F(WorkerTest, HandlerRegistryFindReturnsNulloptForUnknown) {
    EXPECT_FALSE(registry.find("nothing").has_value());
}

// ─── Single job — happy path ──────────────────────────────────────────────────

TEST_F(WorkerTest, WorkerExecutesSuccessfulJob) {
    std::atomic<bool> executed{false};
    registry.register_handler("ok_job", [&](const Job&) {
        executed.store(true);
    });

    manager->submit_job("ok_job", "", Priority::NORMAL);

    Worker w(1, *manager, registry);
    w.start();

    EXPECT_TRUE(wait_until([&] { return executed.load(); }));
    EXPECT_TRUE(wait_until([&] {
        return manager->get_metrics().jobs_completed == 1;
    }));

    manager->shutdown();

    auto s = w.stats();
    EXPECT_EQ(s.jobs_executed,  1u);
    EXPECT_EQ(s.jobs_succeeded, 1u);
    EXPECT_EQ(s.jobs_failed,    0u);
}

// ─── Job failure and retry ────────────────────────────────────────────────────

TEST_F(WorkerTest, FailingJobWithRetriesGetsRetried) {
    std::atomic<int> call_count{0};

    // Fail on attempt 1; succeed on attempt 2.
    registry.register_handler("flaky", [&](const Job& job) {
        ++call_count;
        if (job.retry_count < 1) {
            throw std::runtime_error("first attempt fails");
        }
    });

    manager->submit_job("flaky", "", Priority::NORMAL, /*max_retries=*/3);

    Worker w(1, *manager, registry);
    w.start();

    // Eventually: 2 executions, 1 completed.
    EXPECT_TRUE(wait_until([&] { return call_count.load() >= 2; }));
    EXPECT_TRUE(wait_until([&] {
        return manager->get_metrics().jobs_completed == 1;
    }));

    manager->shutdown();

    EXPECT_GE(call_count.load(), 2);
    EXPECT_EQ(manager->get_metrics().jobs_retried,   1u);
    EXPECT_EQ(manager->get_metrics().jobs_completed, 1u);
    EXPECT_EQ(manager->get_metrics().jobs_failed,    0u);
}

TEST_F(WorkerTest, PermanentlyFailingJobExhaustsRetries) {
    registry.register_handler("doomed", [](const Job&) {
        throw std::runtime_error("always fails");
    });

    // max_retries = 2 → total 3 attempts (0, 1, 2).
    manager->submit_job("doomed", "", Priority::NORMAL, /*max_retries=*/2);

    Worker w(1, *manager, registry);
    w.start();

    EXPECT_TRUE(wait_until([&] {
        auto m = manager->get_metrics();
        return m.jobs_failed == 1 || m.jobs_completed == 1;
    }, 5000ms));

    manager->shutdown();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_failed,    1u);
    EXPECT_EQ(m.jobs_completed, 0u);
}

// ─── Unregistered handler ─────────────────────────────────────────────────────

TEST_F(WorkerTest, UnregisteredHandlerCausesPermanentFailure) {
    // No handler for "ghost_type" — should fail immediately without retry.
    manager->submit_job("ghost_type", "", Priority::NORMAL, 5);

    Worker w(1, *manager, registry);
    w.start();

    EXPECT_TRUE(wait_until([&] {
        auto m = manager->get_metrics();
        return m.jobs_failed == 1 || m.jobs_completed == 1;
    }));

    manager->shutdown();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_failed,  1u);
    EXPECT_EQ(m.queue_length, 0u);
}

// ─── Multiple workers — concurrency ───────────────────────────────────────────

TEST_F(WorkerTest, MultipleWorkersProcessJobsConcurrently) {
    constexpr int NUM_JOBS    = 20;
    constexpr int NUM_WORKERS = 4;

    std::atomic<int> total_executed{0};

    registry.register_handler("concurrent_job", [&](const Job&) {
        std::this_thread::sleep_for(20ms);
        ++total_executed;
    });

    for (int i = 0; i < NUM_JOBS; ++i) {
        manager->submit_job("concurrent_job", std::to_string(i));
    }

    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 1; i <= NUM_WORKERS; ++i) {
        workers.push_back(std::make_unique<Worker>(i, *manager, registry));
        workers.back()->start();
    }

    EXPECT_TRUE(wait_until([&] {
        return manager->get_metrics().jobs_completed == NUM_JOBS;
    }, 10000ms));

    manager->shutdown();
    for (auto& w : workers) w->stop();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_completed, static_cast<uint64_t>(NUM_JOBS));
    EXPECT_EQ(m.jobs_failed,    0u);
    EXPECT_EQ(m.queue_length,   0u);
    EXPECT_EQ(total_executed.load(), NUM_JOBS);
}

// ─── Priority under concurrent load ──────────────────────────────────────────

TEST_F(WorkerTest, HighPriorityJobsExecutedBeforeLowWhenQueueHasBacklog) {
    // Saturate queue before starting workers.
    // The single worker should pick up HIGH first.
    for (int i = 0; i < 5; ++i) {
        manager->submit_job("prio_job", "LOW",  Priority::LOW,  1);
    }
    for (int i = 0; i < 3; ++i) {
        manager->submit_job("prio_job", "HIGH", Priority::HIGH, 1);
    }

    // Capture execution order.
    std::vector<std::string> order;
    std::mutex               order_mutex;

    registry.register_handler("prio_job", [&](const Job& job) {
        std::lock_guard lk{order_mutex};
        order.push_back(job.payload);
    });

    Worker w(1, *manager, registry);
    w.start();

    EXPECT_TRUE(wait_until([&] {
        return manager->get_metrics().jobs_completed == 8;
    }));

    manager->shutdown();

    // The first 3 completed jobs should all be HIGH priority.
    std::lock_guard lk{order_mutex};
    ASSERT_GE(order.size(), 3u);
    EXPECT_EQ(order[0], "HIGH");
    EXPECT_EQ(order[1], "HIGH");
    EXPECT_EQ(order[2], "HIGH");
}

// ─── Worker shutdown ──────────────────────────────────────────────────────────

TEST_F(WorkerTest, WorkerStopsCleanlyOnShutdown) {
    registry.register_handler("slow", [](const Job&) {
        std::this_thread::sleep_for(50ms);
    });

    for (int i = 0; i < 3; ++i) manager->submit_job("slow");

    Worker w(1, *manager, registry);
    w.start();

    std::this_thread::sleep_for(30ms);  // let it start one job

    manager->shutdown();
    w.stop();  // should return promptly (not hang)

    EXPECT_FALSE(w.running());
}
