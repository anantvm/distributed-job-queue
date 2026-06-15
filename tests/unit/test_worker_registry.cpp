// tests/unit/test_worker_registry.cpp
//
// Unit tests for WorkerRegistry (Phase 3)
// ─────────────────────────────────────────────────────────────────────────────

#include <manager/worker_registry.hpp>

#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

// ─── Registration ─────────────────────────────────────────────────────────────

TEST(WorkerRegistry, RegisterAndFind) {
    WorkerRegistry reg;
    reg.register_worker("w1", "127.0.0.1");

    auto info = reg.get_worker("w1");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->worker_id, "w1");
    EXPECT_EQ(info->host, "127.0.0.1");
    EXPECT_EQ(reg.size(), 1);
}

TEST(WorkerRegistry, DeregisterRemovesWorker) {
    WorkerRegistry reg;
    reg.register_worker("w1");
    reg.deregister_worker("w1");

    EXPECT_FALSE(reg.get_worker("w1").has_value());
    EXPECT_EQ(reg.size(), 0);
}

TEST(WorkerRegistry, ReregisterReplacesOldEntry) {
    // Simulates worker reconnection after crash with same ID.
    WorkerRegistry reg;
    reg.register_worker("w1");
    reg.assign_job("w1", "job-A");

    // Re-register (crash recovery).
    reg.register_worker("w1");

    auto info = reg.get_worker("w1");
    ASSERT_TRUE(info.has_value());
    // Old assigned_jobs are cleared on re-registration.
    EXPECT_TRUE(info->assigned_jobs.empty());
}

// ─── Heartbeat ────────────────────────────────────────────────────────────────

TEST(WorkerRegistry, HeartbeatUpdatesTimestamp) {
    WorkerRegistry reg;
    reg.register_worker("w1");

    auto before = reg.get_worker("w1")->last_heartbeat_ms;
    std::this_thread::sleep_for(10ms);
    reg.record_heartbeat("w1");
    auto after = reg.get_worker("w1")->last_heartbeat_ms;

    EXPECT_GT(after, before);
}

TEST(WorkerRegistry, HeartbeatOnUnknownWorkerIsNoOp) {
    WorkerRegistry reg;
    // Should not throw or crash.
    EXPECT_NO_THROW(reg.record_heartbeat("nonexistent"));
}

// ─── Job assignment ───────────────────────────────────────────────────────────

TEST(WorkerRegistry, AssignAndReleaseJob) {
    WorkerRegistry reg;
    reg.register_worker("w1");

    reg.assign_job("w1", "job-1");
    reg.assign_job("w1", "job-2");

    EXPECT_EQ(reg.get_worker("w1")->job_count(), 2);
    EXPECT_FALSE(reg.get_worker("w1")->is_idle());

    reg.release_job("w1", "job-1");
    EXPECT_EQ(reg.get_worker("w1")->job_count(), 1);

    reg.release_job("w1", "job-2");
    EXPECT_TRUE(reg.get_worker("w1")->is_idle());
}

TEST(WorkerRegistry, JobsOfWorker) {
    WorkerRegistry reg;
    reg.register_worker("w1");
    reg.assign_job("w1", "job-A");
    reg.assign_job("w1", "job-B");

    auto jobs = reg.jobs_of_worker("w1");
    EXPECT_EQ(jobs.size(), 2u);
    EXPECT_TRUE(jobs.count("job-A"));
    EXPECT_TRUE(jobs.count("job-B"));
}

// ─── Failure detection ────────────────────────────────────────────────────────

TEST(WorkerRegistry, DetectFailedReturnsStaleWorkers) {
    WorkerRegistry reg;
    reg.register_worker("fresh");
    reg.register_worker("stale");

    // Backdate stale worker's heartbeat by overwriting via record_heartbeat
    // is not possible directly — use the public API by setting a very short
    // timeout and letting real time pass.
    std::this_thread::sleep_for(20ms);
    // Only "fresh" gets a new heartbeat.
    reg.record_heartbeat("fresh");

    // Timeout = 10ms; "stale" last heartbeat was ~20ms+ ago.
    auto failed = reg.detect_failed(10);
    EXPECT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0], "stale");
}

TEST(WorkerRegistry, DetectFailedIgnoresFreshWorkers) {
    WorkerRegistry reg;
    reg.register_worker("w1");
    reg.record_heartbeat("w1");

    // Very large timeout — nothing should be stale.
    auto failed = reg.detect_failed(60'000);
    EXPECT_TRUE(failed.empty());
}

// ─── List workers ─────────────────────────────────────────────────────────────

TEST(WorkerRegistry, ListWorkersReturnsSortedByRegistration) {
    WorkerRegistry reg;
    reg.register_worker("w1");
    std::this_thread::sleep_for(5ms);
    reg.register_worker("w2");
    std::this_thread::sleep_for(5ms);
    reg.register_worker("w3");

    auto list = reg.list_workers();
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].worker_id, "w1");
    EXPECT_EQ(list[1].worker_id, "w2");
    EXPECT_EQ(list[2].worker_id, "w3");
}

// ─── Thread safety ────────────────────────────────────────────────────────────

TEST(WorkerRegistry, ConcurrentAccessIsSafe) {
    WorkerRegistry reg;
    // Stress: concurrent register/heartbeat/list calls.
    constexpr int N = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&reg, i] {
            std::string id = "w" + std::to_string(i);
            reg.register_worker(id);
            reg.record_heartbeat(id);
            reg.assign_job(id, "job-" + std::to_string(i));
            reg.list_workers();
            reg.detect_failed(60'000);
            reg.release_job(id, "job-" + std::to_string(i));
            reg.deregister_worker(id);
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(reg.size(), 0);
}
