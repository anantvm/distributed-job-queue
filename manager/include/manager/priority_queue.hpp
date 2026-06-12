#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// priority_queue.hpp
//
// Thread-safe max-priority queue for MPMC job dispatch.
//
// WaitAndPop() uses a timed wait loop so that the stop flag is checked
// periodically — compatible with Apple Clang 14 which doesn't ship the
// C++20 stop_token library extensions for condition variables.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/stop_source.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

class ThreadSafePriorityQueue {
public:
    ThreadSafePriorityQueue()  = default;
    ~ThreadSafePriorityQueue() = default;

    // Non-copyable and non-movable
    ThreadSafePriorityQueue(const ThreadSafePriorityQueue&)            = delete;
    ThreadSafePriorityQueue& operator=(const ThreadSafePriorityQueue&) = delete;

    // ── Producers ────────────────────────────────────────────────────────────

    // Push a job and wake one waiting consumer.
    void push(Job job);

    // ── Consumers ────────────────────────────────────────────────────────────

    // Non-blocking: returns nullopt immediately if the queue is empty.
    [[nodiscard]] std::optional<Job> try_pop();

    // Blocking: sleeps (with periodic wakeup) until a job is available,
    // the queue shuts down, or the stop token is triggered.
    [[nodiscard]] std::optional<Job> wait_and_pop(const StopToken& stop_tok);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Signal all waiting consumers to wake up and return nullopt.
    // After calling shutdown(), push() becomes a no-op.
    void shutdown();

    // ── Observers ────────────────────────────────────────────────────────────

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool        empty() const;
    [[nodiscard]] bool        is_shutdown() const;

private:
    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::priority_queue<Job> queue_;
    bool                    shutdown_{false};
};
