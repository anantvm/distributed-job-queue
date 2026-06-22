// tests/unit/test_latency_tracker.cpp
#include <metrics/latency_tracker.hpp>
#include <metrics/metrics_registry.hpp>
#include <gtest/gtest.h>
#include <chrono>

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

class LatencyTrackerTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().reset_all();
    }
};

TEST_F(LatencyTrackerTest, CompleteJobFeedsHistogram) {
    LatencyTracker::instance().reset();

    int64_t t0 = now_ms();
    LatencyTracker::instance().on_submitted("job1", t0);
    LatencyTracker::instance().on_dispatched("job1", t0 + 10);
    LatencyTracker::instance().on_completed("job1", t0 + 50);

    // The e2e histogram should have 1 observation with sum ~= 50ms
    auto& h = MetricsRegistry::instance().histogram("job_end_to_end_ms");
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 1u);
    EXPECT_NEAR(s.sum, 50.0, 5.0);  // allow ±5ms for clock imprecision
}

TEST_F(LatencyTrackerTest, DispatchToCompleteHistogram) {
    LatencyTracker::instance().reset();

    int64_t t0 = now_ms();
    LatencyTracker::instance().on_submitted("job2", t0);
    LatencyTracker::instance().on_dispatched("job2", t0 + 20);
    LatencyTracker::instance().on_completed("job2", t0 + 120);

    auto& h = MetricsRegistry::instance().histogram("job_dispatch_to_complete_ms");
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 1u);
    EXPECT_NEAR(s.sum, 100.0, 5.0);
}

TEST_F(LatencyTrackerTest, SubmitToDispatchHistogram) {
    LatencyTracker::instance().reset();

    int64_t t0 = now_ms();
    LatencyTracker::instance().on_submitted("job3", t0);
    LatencyTracker::instance().on_dispatched("job3", t0 + 30);
    LatencyTracker::instance().on_completed("job3", t0 + 80);

    auto& h = MetricsRegistry::instance().histogram("job_submit_to_dispatch_ms");
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 1u);
    EXPECT_NEAR(s.sum, 30.0, 5.0);
}

TEST_F(LatencyTrackerTest, FailedJobRemovedWithoutFeedingHistogram) {
    LatencyTracker::instance().reset();

    int64_t t0 = now_ms();
    LatencyTracker::instance().on_submitted("job4", t0);
    LatencyTracker::instance().on_failed("job4", t0 + 10);

    // No histogram observations expected
    auto& h = MetricsRegistry::instance().histogram("job_end_to_end_ms");
    EXPECT_EQ(h.snapshot().count, 0u);
}

TEST_F(LatencyTrackerTest, PurgeRemovesOldEntries) {
    LatencyTracker::instance().reset();

    int64_t old_ts = now_ms() - 1'000'000;  // 1000 seconds ago
    LatencyTracker::instance().on_submitted("old_job", old_ts);

    // Purge entries older than 500 seconds ago
    LatencyTracker::instance().purge_older_than(now_ms() - 500'000);

    // The old job should be gone — completing it now should not feed histogram
    LatencyTracker::instance().on_completed("old_job", now_ms());
    auto& h = MetricsRegistry::instance().histogram("job_end_to_end_ms");
    EXPECT_EQ(h.snapshot().count, 0u);
}

TEST_F(LatencyTrackerTest, MultipleJobsCorrectlyAccumulate) {
    LatencyTracker::instance().reset();

    int64_t t0 = now_ms();
    for (int i = 0; i < 10; ++i) {
        std::string id = "job" + std::to_string(i);
        LatencyTracker::instance().on_submitted(id, t0);
        LatencyTracker::instance().on_dispatched(id, t0 + 10);
        LatencyTracker::instance().on_completed(id, t0 + 100);
    }

    auto& h = MetricsRegistry::instance().histogram("job_end_to_end_ms");
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 10u);
    EXPECT_NEAR(s.mean(), 100.0, 5.0);
}
