// tests/unit/test_storage.cpp
//
// Unit tests for both storage backends.
// SQLite backend uses ":memory:" — no filesystem artefacts.

#include <storage/in_memory_backend.hpp>
#include <storage/sqlite_backend.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Job make_job(const std::string& id,
                    JobStatus status   = JobStatus::PENDING,
                    Priority  priority = Priority::NORMAL) {
    Job j;
    j.job_id        = id;
    j.job_type      = "test_type";
    j.payload       = R"({"key":"value"})";
    j.priority      = priority;
    j.status        = status;
    j.created_at_ms = duration_cast<milliseconds>(
                          system_clock::now().time_since_epoch()).count();
    j.updated_at_ms = j.created_at_ms;
    j.max_retries   = 3;
    j.retry_count   = 0;
    return j;
}

// ─── Typed test fixture ───────────────────────────────────────────────────────
// We run the same suite against both InMemoryBackend and SQLiteBackend.

template <typename BackendType>
class StorageTest : public ::testing::Test {
protected:
    std::unique_ptr<IStorageBackend> backend;

    void SetUp() override {
        if constexpr (std::is_same_v<BackendType, SQLiteBackend>) {
            backend = std::make_unique<SQLiteBackend>(":memory:");
        } else {
            backend = std::make_unique<InMemoryBackend>();
        }
    }
};

using BackendTypes = ::testing::Types<InMemoryBackend, SQLiteBackend>;
TYPED_TEST_SUITE(StorageTest, BackendTypes);

// ─── persist_job ─────────────────────────────────────────────────────────────

TYPED_TEST(StorageTest, PersistNewJob) {
    auto r = this->backend->persist_job(make_job("job-1"));
    EXPECT_TRUE(r.ok()) << r.error();
}

TYPED_TEST(StorageTest, PersistDuplicateJobFails) {
    ASSERT_TRUE(this->backend->persist_job(make_job("dup")).ok());
    auto r = this->backend->persist_job(make_job("dup"));
    EXPECT_TRUE(r.err());  // duplicate primary key
}

// ─── get_job ─────────────────────────────────────────────────────────────────

TYPED_TEST(StorageTest, GetJobReturnsCorrectJob) {
    auto job = make_job("job-2", JobStatus::PENDING, Priority::HIGH);
    job.payload = "my payload";
    ASSERT_TRUE(this->backend->persist_job(job).ok());

    auto result = this->backend->get_job("job-2");
    ASSERT_TRUE(result.ok())               << result.error();
    ASSERT_TRUE(result.value().has_value()) << "Expected job to exist";

    const auto& found = result.value().value();
    EXPECT_EQ(found.job_id,   "job-2");
    EXPECT_EQ(found.priority, Priority::HIGH);
    EXPECT_EQ(found.payload,  "my payload");
    EXPECT_EQ(found.status,   JobStatus::PENDING);
}

TYPED_TEST(StorageTest, GetJobMissingReturnsNullopt) {
    auto result = this->backend->get_job("does-not-exist");
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().has_value());
}

// ─── update_status ───────────────────────────────────────────────────────────

TYPED_TEST(StorageTest, UpdateStatusToRunning) {
    ASSERT_TRUE(this->backend->persist_job(make_job("job-3")).ok());

    auto r = this->backend->update_status("job-3", JobStatus::RUNNING);
    ASSERT_TRUE(r.ok()) << r.error();

    auto found = this->backend->get_job("job-3");
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().status, JobStatus::RUNNING);
}

TYPED_TEST(StorageTest, UpdateStatusToCompletedWithError) {
    ASSERT_TRUE(this->backend->persist_job(make_job("job-4")).ok());

    auto r = this->backend->update_status("job-4", JobStatus::FAILED, "boom");
    ASSERT_TRUE(r.ok()) << r.error();

    auto found = this->backend->get_job("job-4");
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().status,     JobStatus::FAILED);
    EXPECT_EQ(found.value().value().last_error, "boom");
}

// ─── increment_retry ─────────────────────────────────────────────────────────

TYPED_TEST(StorageTest, IncrementRetryBumpsCountAndResetsToPending) {
    auto job = make_job("job-5");
    job.status      = JobStatus::RUNNING;
    job.retry_count = 0;
    ASSERT_TRUE(this->backend->persist_job(job).ok());

    auto r = this->backend->increment_retry("job-5");
    ASSERT_TRUE(r.ok()) << r.error();

    auto found = this->backend->get_job("job-5");
    ASSERT_TRUE(found.ok() && found.value().has_value());
    EXPECT_EQ(found.value().value().retry_count, 1);
    EXPECT_EQ(found.value().value().status, JobStatus::PENDING);
}

// ─── load_recoverable_jobs ───────────────────────────────────────────────────

TYPED_TEST(StorageTest, LoadRecoverableReturnsOnlyPendingAndRunning) {
    ASSERT_TRUE(this->backend->persist_job(make_job("p1", JobStatus::PENDING)).ok());
    ASSERT_TRUE(this->backend->persist_job(make_job("p2", JobStatus::RUNNING)).ok());
    ASSERT_TRUE(this->backend->persist_job(make_job("p3", JobStatus::COMPLETED)).ok());
    ASSERT_TRUE(this->backend->persist_job(make_job("p4", JobStatus::FAILED)).ok());

    auto result = this->backend->load_recoverable_jobs();
    ASSERT_TRUE(result.ok()) << result.error();

    const auto& jobs = result.value();
    EXPECT_EQ(jobs.size(), 2u);

    // Both should be PENDING or RUNNING.
    for (const auto& j : jobs) {
        EXPECT_TRUE(j.status == JobStatus::PENDING || j.status == JobStatus::RUNNING);
    }
}

TYPED_TEST(StorageTest, LoadRecoverableIsEmptyWhenNoJobs) {
    auto result = this->backend->load_recoverable_jobs();
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

// ─── total_job_count ─────────────────────────────────────────────────────────

TYPED_TEST(StorageTest, TotalJobCount) {
    EXPECT_EQ(this->backend->total_job_count().value(), 0);

    ASSERT_TRUE(this->backend->persist_job(make_job("c1")).ok());
    ASSERT_TRUE(this->backend->persist_job(make_job("c2")).ok());
    ASSERT_TRUE(this->backend->persist_job(make_job("c3")).ok());

    EXPECT_EQ(this->backend->total_job_count().value(), 3);
}
