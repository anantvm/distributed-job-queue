// manager/src/priority_queue.cpp
#include <manager/priority_queue.hpp>

#include <chrono>

using namespace std::chrono_literals;

// ─── push ─────────────────────────────────────────────────────────────────────

void ThreadSafePriorityQueue::push(Job job) {
    {
        std::lock_guard<std::mutex> lk{mutex_};
        if (shutdown_) return;
        queue_.push(std::move(job));
    }
    cv_.notify_one();
}

// ─── try_pop ─────────────────────────────────────────────────────────────────

std::optional<Job> ThreadSafePriorityQueue::try_pop() {
    std::lock_guard<std::mutex> lk{mutex_};
    if (queue_.empty()) return std::nullopt;
    Job top = queue_.top();
    queue_.pop();
    return top;
}

// ─── wait_and_pop ────────────────────────────────────────────────────────────
//
// Uses a 50ms timed wait loop so that the stop flag is polled without
// spinning at 100% CPU.  50ms latency on shutdown is acceptable for Phase 1.

std::optional<Job> ThreadSafePriorityQueue::wait_and_pop(const StopToken& stop_tok) {
    while (!stop_tok.stop_requested()) {
        std::unique_lock<std::mutex> lk{mutex_};

        // Wait up to 50ms for a job or a shutdown signal.
        cv_.wait_for(lk, 50ms, [this] {
            return !queue_.empty() || shutdown_;
        });

        if (!queue_.empty()) {
            Job top = queue_.top();
            queue_.pop();
            return top;
        }

        if (shutdown_) return std::nullopt;
        // else: spurious wakeup or timeout — re-check stop flag in loop header
    }
    return std::nullopt;  // stop was requested
}

// ─── shutdown ────────────────────────────────────────────────────────────────

void ThreadSafePriorityQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lk{mutex_};
        shutdown_ = true;
    }
    cv_.notify_all();
}

// ─── Observers ────────────────────────────────────────────────────────────────

std::size_t ThreadSafePriorityQueue::size() const {
    std::lock_guard<std::mutex> lk{mutex_};
    return queue_.size();
}

bool ThreadSafePriorityQueue::empty() const {
    std::lock_guard<std::mutex> lk{mutex_};
    return queue_.empty();
}

bool ThreadSafePriorityQueue::is_shutdown() const {
    std::lock_guard<std::mutex> lk{mutex_};
    return shutdown_;
}
