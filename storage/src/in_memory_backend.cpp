// storage/src/in_memory_backend.cpp
#include <storage/in_memory_backend.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ─── persist_job ─────────────────────────────────────────────────────────────

VoidResult InMemoryBackend::persist_job(const Job& job) {
    std::lock_guard lk{mutex_};
    if (jobs_.count(job.job_id)) {
        return VoidResult::Err("persist_job: duplicate job_id " + job.job_id);
    }
    jobs_[job.job_id] = job;
    return VoidResult::Ok();
}

// ─── update_status ───────────────────────────────────────────────────────────

VoidResult InMemoryBackend::update_status(
    const std::string& job_id,
    JobStatus           status,
    const std::string&  error) {

    std::lock_guard lk{mutex_};
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return VoidResult::Err("update_status: job not found: " + job_id);
    }
    it->second.status     = status;
    it->second.last_error = error;
    return VoidResult::Ok();
}

// ─── increment_retry ─────────────────────────────────────────────────────────

VoidResult InMemoryBackend::increment_retry(const std::string& job_id) {
    std::lock_guard lk{mutex_};
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return VoidResult::Err("increment_retry: job not found: " + job_id);
    }
    it->second.retry_count++;
    it->second.status = JobStatus::PENDING;
    return VoidResult::Ok();
}

// ─── load_recoverable_jobs ───────────────────────────────────────────────────

Result<std::vector<Job>> InMemoryBackend::load_recoverable_jobs() {
    std::lock_guard lk{mutex_};
    std::vector<Job> result;
    result.reserve(jobs_.size());
    for (const auto& [id, job] : jobs_) {
        if (job.status == JobStatus::PENDING || job.status == JobStatus::RUNNING) {
            result.push_back(job);
        }
    }
    return Result<std::vector<Job>>::Ok(std::move(result));
}

// ─── get_job ─────────────────────────────────────────────────────────────────

Result<std::optional<Job>> InMemoryBackend::get_job(const std::string& job_id) {
    std::lock_guard lk{mutex_};
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return Result<std::optional<Job>>::Ok(std::nullopt);
    }
    return Result<std::optional<Job>>::Ok(it->second);
}

// ─── total_job_count ─────────────────────────────────────────────────────────

Result<int64_t> InMemoryBackend::total_job_count() {
    std::lock_guard lk{mutex_};
    return Result<int64_t>::Ok(static_cast<int64_t>(jobs_.size()));
}

// ─── Test helpers ─────────────────────────────────────────────────────────────

void InMemoryBackend::clear() {
    std::lock_guard lk{mutex_};
    jobs_.clear();
}

size_t InMemoryBackend::size() const {
    std::lock_guard lk{mutex_};
    return jobs_.size();
}
