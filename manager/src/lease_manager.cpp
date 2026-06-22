// manager/src/lease_manager.cpp
#include <manager/lease_manager.hpp>

#include <chrono>

using namespace std::chrono_literals;

// ─── now_ms ──────────────────────────────────────────────────────────────────

int64_t LeaseManager::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─── Destructor ───────────────────────────────────────────────────────────────

LeaseManager::~LeaseManager() { stop(); }

// ─── start ───────────────────────────────────────────────────────────────────

void LeaseManager::start(ExpireCallback on_expire) {
    on_expire_ = std::move(on_expire);
    running_   = true;
    checker_thread_ = std::thread([this] { checker_loop(); });
}

// ─── stop ────────────────────────────────────────────────────────────────────

void LeaseManager::stop() {
    running_ = false;
    cv_.notify_all();              // wake checker so it exits immediately
    if (checker_thread_.joinable()) checker_thread_.join();

    std::unique_lock lk{mu_in_flight_};
    cv_in_flight_.wait(lk, [this] { return in_flight_.load() == 0; });
}

// ─── grant ───────────────────────────────────────────────────────────────────

void LeaseManager::grant(const std::string& job_id,
                         const std::string& worker_id,
                         int64_t duration_ms) {
    Lease lease;
    lease.job_id       = job_id;
    lease.worker_id    = worker_id;
    lease.expires_at_ms = now_ms() + duration_ms;

    std::unique_lock lk{mu_};
    leases_[job_id] = std::move(lease);
}

// ─── revoke ──────────────────────────────────────────────────────────────────

bool LeaseManager::revoke(const std::string& job_id) {
    std::unique_lock lk{mu_};
    return leases_.erase(job_id) > 0;
}

// ─── active_lease_count ──────────────────────────────────────────────────────

int LeaseManager::active_lease_count() const {
    std::unique_lock lk{mu_};
    return static_cast<int>(leases_.size());
}

// ─── list_leases ─────────────────────────────────────────────────────────────

std::vector<LeaseManager::LeaseInfo> LeaseManager::list_leases() const {
    std::unique_lock lk{mu_};
    std::vector<LeaseInfo> out;
    out.reserve(leases_.size());
    for (const auto& [id, l] : leases_)
        out.push_back({l.job_id, l.worker_id, l.expires_at_ms});
    return out;
}

// ─── checker_loop ────────────────────────────────────────────────────────────
// Wakes every kCheckIntervalMs (1 second), collects expired leases, fires
// callbacks OUTSIDE the mutex to avoid re-entrancy issues.

void LeaseManager::checker_loop() {
    while (running_) {
        // Wait for kCheckIntervalMs or until notified by stop().
        {
            std::unique_lock lk{mu_};
            cv_.wait_for(lk, std::chrono::milliseconds(kCheckIntervalMs),
                         [this] { return !running_.load(); });
        }
        if (!running_) break;

        // Collect expired leases without holding the lock during callbacks.
        std::vector<Lease> expired;
        {
            std::unique_lock lk{mu_};
            int64_t now = now_ms();
            for (auto it = leases_.begin(); it != leases_.end(); ) {
                if (it->second.expires_at_ms <= now) {
                    expired.push_back(it->second);
                    it = leases_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Fire callbacks outside the lock — safe for re-entrant revoke()/grant().
        if (on_expire_) {
            for (const auto& l : expired) {
                in_flight_++;
                try {
                    on_expire_(l.job_id, l.worker_id);
                } catch (...) {}
                in_flight_--;
                if (in_flight_.load() == 0) cv_in_flight_.notify_all();
            }
        }
    }
}
