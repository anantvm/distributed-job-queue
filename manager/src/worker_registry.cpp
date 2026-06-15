// manager/src/worker_registry.cpp
#include <manager/worker_registry.hpp>

#include <chrono>
#include <algorithm>

// ─── now_ms ──────────────────────────────────────────────────────────────────

int64_t WorkerRegistry::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─── register_worker ─────────────────────────────────────────────────────────

void WorkerRegistry::register_worker(const std::string& worker_id,
                                     const std::string& host) {
    std::unique_lock lk{mu_};
    WorkerInfo info;
    info.worker_id         = worker_id;
    info.host              = host;
    info.registered_at_ms  = now_ms();
    info.last_heartbeat_ms = info.registered_at_ms;
    workers_[worker_id] = std::move(info);
}

// ─── deregister_worker ───────────────────────────────────────────────────────

void WorkerRegistry::deregister_worker(const std::string& worker_id) {
    std::unique_lock lk{mu_};
    workers_.erase(worker_id);
}

// ─── record_heartbeat ─────────────────────────────────────────────────────────

void WorkerRegistry::record_heartbeat(const std::string& worker_id) {
    std::unique_lock lk{mu_};
    auto it = workers_.find(worker_id);
    if (it != workers_.end())
        it->second.last_heartbeat_ms = now_ms();
}

// ─── assign_job ──────────────────────────────────────────────────────────────

void WorkerRegistry::assign_job(const std::string& worker_id,
                                 const std::string& job_id) {
    std::unique_lock lk{mu_};
    auto it = workers_.find(worker_id);
    if (it != workers_.end())
        it->second.assigned_jobs.insert(job_id);
}

// ─── release_job ─────────────────────────────────────────────────────────────

void WorkerRegistry::release_job(const std::string& worker_id,
                                  const std::string& job_id) {
    std::unique_lock lk{mu_};
    auto it = workers_.find(worker_id);
    if (it != workers_.end())
        it->second.assigned_jobs.erase(job_id);
}

// ─── detect_failed ───────────────────────────────────────────────────────────
// Returns worker_ids whose last heartbeat is older than timeout_ms.
// Uses a shared lock — multiple threads can call this simultaneously.

std::vector<std::string> WorkerRegistry::detect_failed(int64_t timeout_ms) const {
    std::shared_lock lk{mu_};
    int64_t cutoff = now_ms() - timeout_ms;
    std::vector<std::string> failed;
    for (const auto& [id, info] : workers_) {
        if (info.last_heartbeat_ms < cutoff)
            failed.push_back(id);
    }
    return failed;
}

// ─── jobs_of_worker ──────────────────────────────────────────────────────────

std::set<std::string> WorkerRegistry::jobs_of_worker(
        const std::string& worker_id) const {
    std::shared_lock lk{mu_};
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return {};
    return it->second.assigned_jobs;
}

// ─── get_worker ──────────────────────────────────────────────────────────────

std::optional<WorkerInfo> WorkerRegistry::get_worker(
        const std::string& worker_id) const {
    std::shared_lock lk{mu_};
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return std::nullopt;
    return it->second;
}

// ─── list_workers ────────────────────────────────────────────────────────────

std::vector<WorkerInfo> WorkerRegistry::list_workers() const {
    std::shared_lock lk{mu_};
    std::vector<WorkerInfo> out;
    out.reserve(workers_.size());
    for (const auto& [id, info] : workers_)
        out.push_back(info);
    // Sort by registration time for deterministic output.
    std::sort(out.begin(), out.end(),
        [](const WorkerInfo& a, const WorkerInfo& b) {
            return a.registered_at_ms < b.registered_at_ms;
        });
    return out;
}

// ─── size ────────────────────────────────────────────────────────────────────

int WorkerRegistry::size() const {
    std::shared_lock lk{mu_};
    return static_cast<int>(workers_.size());
}
