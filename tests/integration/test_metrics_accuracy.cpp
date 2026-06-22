// tests/integration/test_metrics_accuracy.cpp
//
// Integration test: Submit N jobs through the full server stack and verify
// that counter values in MetricsRegistry match observed job outcomes.
//
// This test embeds a Server and drives it directly via JobManager calls
// (no TCP needed) to avoid port conflicts with other test executables.

#include <metrics/metrics_registry.hpp>
#include <metrics/latency_tracker.hpp>
#include <manager/job_manager.hpp>
#include <storage/sqlite_backend.hpp>

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace std::chrono_literals;

class MetricsAccuracyTest : public ::testing::Test {
protected:
    std::string db_path_;
    std::unique_ptr<JobManager> mgr_;

    void SetUp() override {
        MetricsRegistry::instance().reset_all();
        LatencyTracker::instance().reset();

        db_path_ = "/tmp/test_metrics_accuracy_" +
                   std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db";
        mgr_ = std::make_unique<JobManager>(std::make_unique<SQLiteBackend>(db_path_));
    }

    void TearDown() override {
        mgr_.reset();
        std::filesystem::remove(db_path_);
    }
};

TEST_F(MetricsAccuracyTest, SubmittedCounterMatchesSubmits) {
    constexpr int kJobs = 20;
    for (int i = 0; i < kJobs; ++i) {
        auto r = mgr_->submit_job("accuracy_test", "payload", Priority::NORMAL);
        ASSERT_TRUE(r.ok()) << r.error();
    }
    auto& c = MetricsRegistry::instance().counter("jobs_submitted_total");
    EXPECT_EQ(c.value(), static_cast<uint64_t>(kJobs));
}

TEST_F(MetricsAccuracyTest, QueueDepthTracksSubmitsAndPulls) {
    auto& g = MetricsRegistry::instance().gauge("job_queue_depth");

    EXPECT_EQ(g.value(), 0);
    auto r1 = mgr_->submit_job("accuracy_test", "p1", Priority::NORMAL);
    auto r2 = mgr_->submit_job("accuracy_test", "p2", Priority::NORMAL);
    ASSERT_TRUE(r1.ok()); ASSERT_TRUE(r2.ok());

    EXPECT_EQ(g.value(), 2);

    // Pull one job — depth should drop.
    auto job = mgr_->try_pull_job();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(g.value(), 1);
}

TEST_F(MetricsAccuracyTest, CompletedCounterMatchesCompletes) {
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        auto r = mgr_->submit_job("accuracy_test", "p", Priority::NORMAL);
        ASSERT_TRUE(r.ok());
        ids.push_back(r.value());
    }

    // Pull and complete all
    for (int i = 0; i < 5; ++i) {
        auto job = mgr_->try_pull_job();
        ASSERT_TRUE(job.has_value());
        auto cr = mgr_->complete_job(job->job_id);
        ASSERT_TRUE(cr.ok());
    }

    auto& c = MetricsRegistry::instance().counter("jobs_completed_total");
    EXPECT_EQ(c.value(), 5u);
}

TEST_F(MetricsAccuracyTest, LatencyHistogramObservationOnComplete) {
    LatencyTracker::instance().reset();

    auto r = mgr_->submit_job("latency_test", "payload", Priority::NORMAL);
    ASSERT_TRUE(r.ok());

    // Simulate a dispatch delay by sleeping briefly
    std::this_thread::sleep_for(10ms);
    auto job = mgr_->try_pull_job();
    ASSERT_TRUE(job.has_value());

    // Simulate execution time
    std::this_thread::sleep_for(20ms);
    auto cr = mgr_->complete_job(job->job_id);
    ASSERT_TRUE(cr.ok());

    // e2e histogram should have 1 observation
    auto& h = MetricsRegistry::instance().histogram("job_end_to_end_ms");
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 1u);
    EXPECT_GT(s.sum, 0.0);  // some measurable latency
}

TEST_F(MetricsAccuracyTest, FailedCounterMatchesPermanentFailures) {
    // Submit a job with max_retries=0 so it goes straight to FAILED
    auto r = mgr_->submit_job("fail_test", "payload", Priority::NORMAL, 0);
    ASSERT_TRUE(r.ok());
    auto job = mgr_->try_pull_job();
    ASSERT_TRUE(job.has_value());

    auto fr = mgr_->fail_job(job->job_id, "test failure", 0, 0);
    ASSERT_TRUE(fr.ok());

    auto& failed_c = MetricsRegistry::instance().counter("jobs_failed_total");
    EXPECT_EQ(failed_c.value(), 1u);

    auto& retry_c = MetricsRegistry::instance().counter("jobs_retried_total");
    EXPECT_EQ(retry_c.value(), 0u);
}

TEST_F(MetricsAccuracyTest, RetriedCounterMatchesRetries) {
    // Job with max_retries=2; fail it twice (retried), third fail = permanent
    auto r = mgr_->submit_job("retry_test", "payload", Priority::NORMAL, 2);
    ASSERT_TRUE(r.ok());

    for (int attempt = 0; attempt <= 2; ++attempt) {
        auto job = mgr_->try_pull_job();
        ASSERT_TRUE(job.has_value()) << "Expected job at attempt " << attempt;
        static_cast<void>(mgr_->fail_job(job->job_id, "test", attempt, 2));
    }

    auto& retry_c = MetricsRegistry::instance().counter("jobs_retried_total");
    EXPECT_EQ(retry_c.value(), 2u);

    auto& failed_c = MetricsRegistry::instance().counter("jobs_failed_total");
    EXPECT_EQ(failed_c.value(), 1u);
}
