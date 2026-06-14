// manager/src/job_manager.cpp
#include <manager/job_manager.hpp>

#include <common/logger.hpp>
#include <common/uuid.hpp>

#include <chrono>
#include <string>

static constexpr const char* kComp = "JobManager";

// ─── Helpers ─────────────────────────────────────────────────────────────────

int64_t JobManager::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

JobManager::JobManager(std::unique_ptr<IStorageBackend> storage)
    : storage_(std::move(storage)) {
    Logger::info(kComp, "JobManager created");
}

JobManager::~JobManager() {
    shutdown();
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

VoidResult JobManager::initialize() {
    Logger::info(kComp, "Initializing — loading recoverable jobs from storage...");

    auto result = storage_->load_recoverable_jobs();
    if (result.err()) {
        return VoidResult::Err("initialize: " + result.error());
    }

    const auto& jobs = result.value();

    // Jobs that were RUNNING when the process last exited have no active
    // worker; reset them to PENDING so they can be re-dispatched.
    int recovered = 0;
    for (auto job : jobs) {
        if (job.status == JobStatus::RUNNING) {
            Logger::warn(kComp,
                "Recovering RUNNING job " + job.job_id + " → PENDING (orphaned lease)");
            job.status = JobStatus::PENDING;
            if (auto r = storage_->update_status(job.job_id, JobStatus::PENDING); r.err()) {
                Logger::error(kComp, "Failed to reset job " + job.job_id + ": " + r.error());
            }
        }
        queue_.push(std::move(job));
        ++recovered;
    }

    Logger::info(kComp, "Recovered " + std::to_string(recovered) + " job(s) from storage");
    return VoidResult::Ok();
}

void JobManager::shutdown() {
    Logger::info(kComp, "Shutting down — signalling all waiting workers");
    queue_.shutdown();
}

// ─── submit_job ──────────────────────────────────────────────────────────────

Result<std::string> JobManager::submit_job(
    const std::string& job_type,
    const std::string& payload,
    Priority           priority,
    int                max_retries) {

    const int64_t ts = now_ms();

    Job job;
    job.job_id        = uuid::generate();
    job.job_type      = job_type;
    job.payload       = payload;
    job.priority      = priority;
    job.status        = JobStatus::PENDING;
    job.created_at_ms = ts;
    job.updated_at_ms = ts;
    job.max_retries   = max_retries;
    job.retry_count   = 0;

    // ── Persist FIRST (write-ahead) ───────────────────────────────────────────
    if (auto r = storage_->persist_job(job); r.err()) {
        return Result<std::string>::Err("submit_job storage error: " + r.error());
    }

    // ── Then enqueue ──────────────────────────────────────────────────────────
    const std::string id = job.job_id;    // capture before move
    queue_.push(std::move(job));

    ++jobs_submitted_;
    Logger::info(kComp,
        "Submitted job " + id +
        " type=" + job_type +
        " priority=" + to_string(priority));

    return Result<std::string>::Ok(id);
}

// ─── Job dispatch ─────────────────────────────────────────────────────────────

std::optional<Job> JobManager::try_pull_job() {
    auto job = queue_.try_pop();
    if (job) {
        // Mark as RUNNING in storage; worker now owns it.
        if (auto r = storage_->update_status(job->job_id, JobStatus::RUNNING); r.err()) {
            Logger::warn(kComp, "Failed to mark job RUNNING: " + r.error());
        }
    }
    return job;
}

std::optional<Job> JobManager::wait_for_job(const StopToken& stop_tok) {
    auto job = queue_.wait_and_pop(stop_tok);
    if (job) {
        if (auto r = storage_->update_status(job->job_id, JobStatus::RUNNING); r.err()) {
            Logger::warn(kComp, "Failed to mark job RUNNING: " + r.error());
        }
    }
    return job;
}

// ─── complete_job ─────────────────────────────────────────────────────────────

VoidResult JobManager::complete_job(const std::string& job_id) {
    if (auto r = storage_->update_status(job_id, JobStatus::COMPLETED); r.err()) {
        return VoidResult::Err("complete_job: " + r.error());
    }
    ++jobs_completed_;
    Logger::info(kComp, "Completed job " + job_id);
    return VoidResult::Ok();
}

// ─── fail_job ────────────────────────────────────────────────────────────────

VoidResult JobManager::fail_job(
    const std::string& job_id,
    const std::string& error,
    int                current_retry_count,
    int                max_retries) {

    if (current_retry_count < max_retries) {
        // ── Retry path ────────────────────────────────────────────────────────
        Logger::warn(kComp,
            "Job " + job_id + " failed (attempt " +
            std::to_string(current_retry_count + 1) + "/" +
            std::to_string(max_retries) + "): " + error +
            " → re-queuing");

        if (auto r = storage_->increment_retry(job_id); r.err()) {
            return VoidResult::Err("fail_job increment_retry: " + r.error());
        }

        // Reload updated job from storage and re-enqueue.
        auto job_result = storage_->get_job(job_id);
        if (job_result.err()) {
            return VoidResult::Err("fail_job get_job: " + job_result.error());
        }
        if (!job_result.value().has_value()) {
            return VoidResult::Err("fail_job: job not found after retry: " + job_id);
        }

        queue_.push(std::move(*job_result.value()));
        ++jobs_retried_;

    } else {
        // ── Permanent failure path ────────────────────────────────────────────
        Logger::error(kComp,
            "Job " + job_id + " exceeded max_retries (" +
            std::to_string(max_retries) + ") → FAILED: " + error);

        if (auto r = storage_->update_status(job_id, JobStatus::FAILED, error); r.err()) {
            return VoidResult::Err("fail_job update_status: " + r.error());
        }
        ++jobs_failed_;
    }

    return VoidResult::Ok();
}

// ─── requeue_job ─────────────────────────────────────────────────────────────
// Called by the server when a worker disconnects mid-execution.
// Does NOT consume a retry slot — the job gets another full chance.

VoidResult JobManager::requeue_job(const std::string& job_id) {
    Logger::warn(kComp,
        "Requeueing job " + job_id + " (worker disconnected — retry count preserved)");

    if (auto r = storage_->update_status(job_id, JobStatus::PENDING); r.err()) {
        return VoidResult::Err("requeue_job update_status: " + r.error());
    }

    auto job_result = storage_->get_job(job_id);
    if (job_result.err())
        return VoidResult::Err("requeue_job get_job: " + job_result.error());
    if (!job_result.value().has_value())
        return VoidResult::Err("requeue_job: job not found: " + job_id);

    queue_.push(std::move(*job_result.value()));
    return VoidResult::Ok();
}

// ─── Metrics ─────────────────────────────────────────────────────────────────

JobManager::Metrics JobManager::get_metrics() const {
    return Metrics{
        .queue_length   = queue_.size(),
        .jobs_submitted = jobs_submitted_.load(std::memory_order_relaxed),
        .jobs_completed = jobs_completed_.load(std::memory_order_relaxed),
        .jobs_failed    = jobs_failed_.load(std::memory_order_relaxed),
        .jobs_retried   = jobs_retried_.load(std::memory_order_relaxed),
    };
}
