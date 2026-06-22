// tests/unit/test_gauge.cpp
#include <metrics/gauge.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

TEST(GaugeTest, StartsAtZero) {
    Gauge g;
    EXPECT_EQ(g.value(), 0);
}

TEST(GaugeTest, Set) {
    Gauge g;
    g.set(42);
    EXPECT_EQ(g.value(), 42);
}

TEST(GaugeTest, Increment) {
    Gauge g;
    g.increment();
    g.increment(4);
    EXPECT_EQ(g.value(), 5);
}

TEST(GaugeTest, Decrement) {
    Gauge g;
    g.set(10);
    g.decrement();
    g.decrement(3);
    EXPECT_EQ(g.value(), 6);
}

TEST(GaugeTest, GoesNegative) {
    Gauge g;
    g.decrement(5);
    EXPECT_EQ(g.value(), -5);
}

TEST(GaugeTest, ConcurrentIncrDecr) {
    // N threads each increment then decrement; should sum to zero.
    Gauge g;
    constexpr int kThreads = 8;
    constexpr int kOps = 50'000;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&g] {
            for (int j = 0; j < kOps; ++j) {
                g.increment();
                g.decrement();
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(g.value(), 0);
}
