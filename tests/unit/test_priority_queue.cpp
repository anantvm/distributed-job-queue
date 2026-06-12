// tests/unit/test_priority_queue.cpp
#include <manager/priority_queue.hpp>
#include <common/stop_source.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Job make_job(const std::string& id, Priority p, int64_t ts_ms = 0) {
    Job j;
    j.job_id        = id;
    j.job_type      = "test";
    j.priority      = p;
    j.created_at_ms = ts_ms;
    return j;
}

// ─── Basic push / pop ─────────────────────────────────────────────────────────

TEST(PriorityQueue, PushAndTryPop) {
    ThreadSafePriorityQueue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);

    q.push(make_job("a", Priority::NORMAL));
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1u);

    auto j = q.try_pop();
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->job_id, "a");
    EXPECT_TRUE(q.empty());
}

TEST(PriorityQueue, TryPopOnEmptyReturnsNullopt) {
    ThreadSafePriorityQueue q;
    EXPECT_FALSE(q.try_pop().has_value());
}

// ─── Priority ordering ────────────────────────────────────────────────────────

TEST(PriorityQueue, HighPriorityComesFirst) {
    ThreadSafePriorityQueue q;
    q.push(make_job("low",    Priority::LOW,    100));
    q.push(make_job("normal", Priority::NORMAL, 200));
    q.push(make_job("high",   Priority::HIGH,   300));

    EXPECT_EQ(q.try_pop()->job_id, "high");
    EXPECT_EQ(q.try_pop()->job_id, "normal");
    EXPECT_EQ(q.try_pop()->job_id, "low");
}

TEST(PriorityQueue, FIFOWithinSamePriority) {
    ThreadSafePriorityQueue q;
    q.push(make_job("newer",  Priority::NORMAL, 200));
    q.push(make_job("older",  Priority::NORMAL, 100));
    q.push(make_job("newest", Priority::NORMAL, 300));

    EXPECT_EQ(q.try_pop()->job_id, "older");
    EXPECT_EQ(q.try_pop()->job_id, "newer");
    EXPECT_EQ(q.try_pop()->job_id, "newest");
}

TEST(PriorityQueue, MixedPriorityAndTimestamp) {
    ThreadSafePriorityQueue q;
    q.push(make_job("low_old",    Priority::LOW,    10));
    q.push(make_job("high_new",   Priority::HIGH,   999));
    q.push(make_job("normal_mid", Priority::NORMAL, 50));
    q.push(make_job("high_old",   Priority::HIGH,   1));

    EXPECT_EQ(q.try_pop()->job_id, "high_old");
    EXPECT_EQ(q.try_pop()->job_id, "high_new");
    EXPECT_EQ(q.try_pop()->job_id, "normal_mid");
    EXPECT_EQ(q.try_pop()->job_id, "low_old");
}

// ─── WaitAndPop with StopToken ────────────────────────────────────────────────

TEST(PriorityQueue, WaitAndPopBlocksThenReturns) {
    ThreadSafePriorityQueue q;

    std::thread producer([&] {
        std::this_thread::sleep_for(30ms);
        q.push(make_job("delayed", Priority::NORMAL));
    });

    StopSource ss;
    auto result = q.wait_and_pop(ss.get_token());
    producer.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->job_id, "delayed");
}

TEST(PriorityQueue, WaitAndPopReturnsNulloptOnShutdown) {
    ThreadSafePriorityQueue q;
    StopSource ss;

    std::optional<Job> result;
    std::thread waiter([&] {
        result = q.wait_and_pop(ss.get_token());
    });

    std::this_thread::sleep_for(30ms);
    q.shutdown();
    waiter.join();

    EXPECT_FALSE(result.has_value());
}

TEST(PriorityQueue, WaitAndPopReturnsNulloptOnStopRequested) {
    ThreadSafePriorityQueue q;
    StopSource ss;

    std::optional<Job> result;
    std::thread waiter([&] {
        result = q.wait_and_pop(ss.get_token());
    });

    std::this_thread::sleep_for(30ms);
    ss.request_stop();
    waiter.join();

    EXPECT_FALSE(result.has_value());
}

// ─── Thread safety — MPMC ────────────────────────────────────────────────────

TEST(PriorityQueue, ConcurrentProducersAndConsumers) {
    ThreadSafePriorityQueue q;
    constexpr int JOBS_PER_PRODUCER = 200;
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    constexpr int TOTAL = JOBS_PER_PRODUCER * PRODUCERS;

    std::atomic<int> consumed{0};
    StopSource stop;

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < JOBS_PER_PRODUCER; ++i) {
                q.push(make_job(std::to_string(p * 1000 + i), Priority::NORMAL));
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < CONSUMERS; ++c) {
        consumers.emplace_back([&] {
            while (consumed.load() < TOTAL) {
                auto j = q.wait_and_pop(stop.get_token());
                if (j.has_value()) ++consumed;
            }
            stop.request_stop();
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(consumed.load(), TOTAL);
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

TEST(PriorityQueue, PushAfterShutdownIsNoOp) {
    ThreadSafePriorityQueue q;
    q.shutdown();
    q.push(make_job("ghost", Priority::HIGH));
    EXPECT_TRUE(q.empty());
}
