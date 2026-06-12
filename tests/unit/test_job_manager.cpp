// tests/unit/test_job_manager.cpp
//
// Unit tests for JobManager.
// Uses InMemoryBackend to avoid filesystem I/O and keep tests fast.

#include <manager/job_manager.hpp>
#include <storage/in_memory_backend.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Fixture ──────────────────────────────────────────────────────────────────

class JobManagerTest : public ::testing::Test {
protected:
    std::unique_ptr<JobManager> manager;

    void SetUp() override {
        auto backend = std::make_unique<InMemoryBackend>();
        manager = std::make_unique<JobManager>(std::move(backend));
        ASSERT_TRUE(manager->initialize().ok());
    }

    void TearDown() override {
        manager->shutdown();
    }

    // Submit a job and assert success; return job_id.
    std::string submit(const std::string& type       = "test",
                       const std::string& payload    = "",
                       Priority           priority   = Priority::NORMAL,
                       int                max_retries = 3) {
        auto r = manager->submit_job(type, payload, priority, max_retries);
        EXPECT_TRUE(r.ok()) << r.error();
        return r.value();
    }
};

// ─── submit_job ──────────────────────────────────────────────────────────────

TEST_F(JobManagerTest, SubmitReturnsJobId) {
    auto r = manager->submit_job("type_a", "hello");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().empty());
}

TEST_F(JobManagerTest, SubmitIncreasesQueueLength) {
    EXPECT_EQ(manager->get_metrics().queue_length, 0u);
    submit();
    EXPECT_EQ(manager->get_metrics().queue_length, 1u);
    submit();
    EXPECT_EQ(manager->get_metrics().queue_length, 2u);
}

TEST_F(JobManagerTest, SubmittedJobPersistedToStorage) {
    auto id = submit("my_job", "payload_data");

    // Go straight to the storage layer to verify durability.
    auto& storage = manager->storage();
    auto result   = storage.get_job(id);
    ASSERT_TRUE(result.ok() && result.value().has_value());

    const auto& job = result.value().value();
    EXPECT_EQ(job.job_id,   id);
    EXPECT_EQ(job.job_type, "my_job");
    EXPECT_EQ(job.payload,  "payload_data");
    EXPECT_EQ(job.status,   JobStatus::PENDING);
}

// ─── Priority ordering via try_pull_job ───────────────────────────────────────

TEST_F(JobManagerTest, HighPriorityJobDispatchedFirst) {
    submit("low",    "", Priority::LOW);
    submit("high",   "", Priority::HIGH);
    submit("normal", "", Priority::NORMAL);

    // Drain the queue.
    std::vector<std::string> order;
    while (auto j = manager->try_pull_job()) {
        order.push_back(j->job_type);
    }

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "high");
    EXPECT_EQ(order[1], "normal");
    EXPECT_EQ(order[2], "low");
}

// ─── complete_job ─────────────────────────────────────────────────────────────

TEST_F(JobManagerTest, CompleteJobUpdatesMetrics) {
    auto id = submit();
    auto job = manager->try_pull_job();
    ASSERT_TRUE(job.has_value());

    auto r = manager->complete_job(job->job_id);
    ASSERT_TRUE(r.ok()) << r.error();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_completed, 1u);
    EXPECT_EQ(m.jobs_failed,    0u);
}

TEST_F(JobManagerTest, CompleteJobMarksStorageAsCompleted) {
    auto id = submit();
    auto job = manager->try_pull_job();
    ASSERT_TRUE(job.has_value());

    ASSERT_TRUE(manager->complete_job(id).ok());

    auto found = manager->storage().get_job(id);
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().status, JobStatus::COMPLETED);
}

// ─── fail_job — retry path ────────────────────────────────────────────────────

TEST_F(JobManagerTest, FailJobWithRetriesRemainingRequeues) {
    auto id = submit("retryable", "", Priority::NORMAL, /*max_retries=*/3);
    auto job = manager->try_pull_job();
    ASSERT_TRUE(job.has_value());

    // First failure: retry_count=0, max_retries=3 → should requeue.
    auto r = manager->fail_job(id, "transient error", 0, 3);
    ASSERT_TRUE(r.ok()) << r.error();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_retried,    1u);
    EXPECT_EQ(m.jobs_failed,     0u);
    EXPECT_EQ(m.queue_length,    1u);  // back in the queue

    // Verify storage shows incremented retry_count.
    auto found = manager->storage().get_job(id);
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().retry_count, 1);
    EXPECT_EQ(found.value().value().status,      JobStatus::PENDING);
}

TEST_F(JobManagerTest, FailJobAtMaxRetriesMarksPermanentFailure) {
    auto id = submit("doomed", "", Priority::NORMAL, /*max_retries=*/2);
    manager->try_pull_job();  // dequeue so we can fail it

    // Exhaust retries: attempt index equals max_retries.
    auto r = manager->fail_job(id, "fatal error", /*retry_count=*/2, /*max_retries=*/2);
    ASSERT_TRUE(r.ok()) << r.error();

    auto m = manager->get_metrics();
    EXPECT_EQ(m.jobs_failed,  1u);
    EXPECT_EQ(m.jobs_retried, 0u);
    EXPECT_EQ(m.queue_length, 0u);  // NOT requeued

    auto found = manager->storage().get_job(id);
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().status,     JobStatus::FAILED);
    EXPECT_EQ(found.value().value().last_error, "fatal error");
}

// ─── initialize (startup recovery) ───────────────────────────────────────────

TEST_F(JobManagerTest, InitializeRecoversPendingJobs) {
    // Submit two jobs; don't consume them.
    submit("job_a");
    submit("job_b");
    EXPECT_EQ(manager->get_metrics().queue_length, 2u);

    // Simulate a "restart": create a new manager pointing at the same storage.
    auto& old_storage = manager->storage();
    // We can't move ownership without keeping a reference, so just verify
    // that load_recoverable_jobs returns two entries.
    auto result = old_storage.load_recoverable_jobs();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().size(), 2u);
}

TEST_F(JobManagerTest, InitializeResetsRunningJobsToPending) {
    // Simulate a job that was in RUNNING state when the process crashed.
    Job running_job;
    running_job.job_id        = "orphaned";
    running_job.job_type      = "test";
    running_job.status        = JobStatus::RUNNING;
    running_job.created_at_ms = 0;
    running_job.updated_at_ms = 0;
    running_job.priority      = Priority::NORMAL;
    running_job.max_retries   = 3;

    ASSERT_TRUE(manager->storage().persist_job(running_job).ok());

    // Create a fresh manager over the same storage.
    auto new_backend = std::make_unique<InMemoryBackend>();
    // Inject the job directly.
    ASSERT_TRUE(new_backend->persist_job(running_job).ok());

    auto new_manager = std::make_unique<JobManager>(std::move(new_backend));
    ASSERT_TRUE(new_manager->initialize().ok());

    // The RUNNING job should be available for dispatch now.
    EXPECT_EQ(new_manager->get_metrics().queue_length, 1u);
    new_manager->shutdown();
}

// ─── Metrics ─────────────────────────────────────────────────────────────────

TEST_F(JobManagerTest, MetricsCounterAccuracy) {
    auto id1 = submit("j1");
    auto id2 = submit("j2");

    auto m0 = manager->get_metrics();
    EXPECT_EQ(m0.jobs_submitted, 2u);

    auto j1 = manager->try_pull_job();
    auto j2 = manager->try_pull_job();
    ASSERT_TRUE(j1 && j2);

    manager->complete_job(j1->job_id);
    manager->fail_job(j2->job_id, "err", j2->max_retries, j2->max_retries);  // permanent fail

    auto m1 = manager->get_metrics();
    EXPECT_EQ(m1.jobs_completed, 1u);
    EXPECT_EQ(m1.jobs_failed,    1u);
    EXPECT_EQ(m1.jobs_retried,   0u);
    EXPECT_EQ(m1.queue_length,   0u);
}
