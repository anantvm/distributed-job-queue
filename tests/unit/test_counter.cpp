// tests/unit/test_counter.cpp
#include <metrics/counter.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

TEST(CounterTest, StartsAtZero) {
    Counter c;
    EXPECT_EQ(c.value(), 0u);
}

TEST(CounterTest, IncrementByOne) {
    Counter c;
    c.increment();
    EXPECT_EQ(c.value(), 1u);
}

TEST(CounterTest, IncrementByN) {
    Counter c;
    c.increment(42);
    EXPECT_EQ(c.value(), 42u);
}

TEST(CounterTest, MultipleIncrements) {
    Counter c;
    c.increment(10);
    c.increment(20);
    c.increment(5);
    EXPECT_EQ(c.value(), 35u);
}

TEST(CounterTest, ResetToZero) {
    Counter c;
    c.increment(100);
    c.reset();
    EXPECT_EQ(c.value(), 0u);
}

TEST(CounterTest, ConcurrentIncrement) {
    Counter c;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100'000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&c] {
            for (int j = 0; j < kPerThread; ++j) c.increment();
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(c.value(), static_cast<uint64_t>(kThreads * kPerThread));
}
