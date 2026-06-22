// tests/unit/test_histogram.cpp
#include <metrics/histogram.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <thread>
#include <vector>

static const std::vector<double> kBuckets = {1, 5, 10, 50, 100, 500, 1000};

TEST(HistogramTest, EmptyHistogram) {
    Histogram h(kBuckets);
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 0u);
    EXPECT_DOUBLE_EQ(s.sum, 0.0);
    // Buckets count = boundaries + 1 for +Inf
    EXPECT_EQ(s.buckets.size(), kBuckets.size() + 1);
    for (const auto& b : s.buckets) {
        EXPECT_EQ(b.cumulative_count, 0u);
    }
}

TEST(HistogramTest, SingleObservation) {
    Histogram h(kBuckets);
    h.observe(7.0);  // falls in bucket <=10
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 1u);
    EXPECT_DOUBLE_EQ(s.sum, 7.0);
    // Buckets <=1 should be 0; buckets >=10 should be 1
    EXPECT_EQ(s.buckets[0].cumulative_count, 0u);  // le=1
    EXPECT_EQ(s.buckets[1].cumulative_count, 0u);  // le=5
    EXPECT_EQ(s.buckets[2].cumulative_count, 1u);  // le=10
    EXPECT_EQ(s.buckets.back().cumulative_count, 1u);  // +Inf
}

TEST(HistogramTest, MultipleObservations) {
    Histogram h(kBuckets);
    h.observe(0.5);   // <= 1
    h.observe(3.0);   // <= 5
    h.observe(8.0);   // <= 10
    h.observe(200.0); // <= 500
    h.observe(2000.0);// > 1000 (only +Inf)

    auto s = h.snapshot();
    EXPECT_EQ(s.count, 5u);
    EXPECT_DOUBLE_EQ(s.sum, 0.5 + 3.0 + 8.0 + 200.0 + 2000.0);

    EXPECT_EQ(s.buckets[0].cumulative_count, 1u);  // le=1
    EXPECT_EQ(s.buckets[1].cumulative_count, 2u);  // le=5
    EXPECT_EQ(s.buckets[2].cumulative_count, 3u);  // le=10
    EXPECT_EQ(s.buckets[3].cumulative_count, 3u);  // le=50
    EXPECT_EQ(s.buckets[4].cumulative_count, 3u);  // le=100
    EXPECT_EQ(s.buckets[5].cumulative_count, 4u);  // le=500
    EXPECT_EQ(s.buckets[6].cumulative_count, 4u);  // le=1000
    EXPECT_EQ(s.buckets.back().cumulative_count, 5u);  // +Inf
}

TEST(HistogramTest, Mean) {
    Histogram h(kBuckets);
    h.observe(10.0);
    h.observe(20.0);
    h.observe(30.0);
    auto s = h.snapshot();
    EXPECT_NEAR(s.mean(), 20.0, 0.001);
}

TEST(HistogramTest, MeanEmptyIsZero) {
    Histogram h(kBuckets);
    EXPECT_DOUBLE_EQ(h.snapshot().mean(), 0.0);
}

TEST(HistogramTest, Reset) {
    Histogram h(kBuckets);
    h.observe(50.0);
    h.observe(100.0);
    h.reset();
    auto s = h.snapshot();
    EXPECT_EQ(s.count, 0u);
    EXPECT_DOUBLE_EQ(s.sum, 0.0);
    for (const auto& b : s.buckets) {
        EXPECT_EQ(b.cumulative_count, 0u);
    }
}

TEST(HistogramTest, P50Reasonable) {
    Histogram h(kBuckets);
    // Observe 100 values: 50 at 5ms (in <=5 bucket), 50 at 50ms (in <=50 bucket)
    for (int i = 0; i < 50; ++i) h.observe(5.0);
    for (int i = 0; i < 50; ++i) h.observe(50.0);
    auto s = h.snapshot();
    double p50 = s.p(0.5);
    // p50 should be between 5 and 50
    EXPECT_GE(p50, 5.0);
    EXPECT_LE(p50, 50.0);
}

TEST(HistogramTest, ConcurrentObserve) {
    Histogram h(kBuckets);
    constexpr int kThreads = 4;
    constexpr int kObs = 10'000;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&h] {
            for (int j = 0; j < kObs; ++j) h.observe(10.0);
        });
    }
    for (auto& t : threads) t.join();
    auto s = h.snapshot();
    EXPECT_EQ(s.count, static_cast<uint64_t>(kThreads * kObs));
}
