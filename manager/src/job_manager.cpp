// manager/src/job_manager.cpp
#include <manager/job_manager.hpp>

#include <common/logger.hpp>
#include <common/uuid.hpp>

// Phase 4: metrics instrumentation
#if __has_include(<metrics/metrics_registry.hpp>)
#  include <metrics/metrics_registry.hpp>
#  include <metrics/latency_tracker.hpp>
#  define METRICS_ENABLED 1
#else
#  define METRICS_ENABLED 0
#endif

#include <chrono>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

static constexpr const char* kComp = "JobManager";

// ─── Helpers ─────────────────────────────────────────────────────────────────

int64_t JobManager::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

JobManager::JobManager(std::unique_ptr<IStorageBackend> storage)
    : storage_(std::move(storage)) {
    if (::pipe(notify_pipe_) < 0) {
        throw std::runtime_error("failed to create notify pipe");
    }
    for (int i = 0; i < 2; ++i) {
        int flags = ::fcntl(notify_pipe_[i], F_GETFL, 0);
        ::fcntl(notify_pipe_[i], F_SETFL, flags | O_NONBLOCK);
    }
    Logger::info(kComp, "JobManager created");
}

JobManager::~JobManager() {
    shutdown();
    if (notify_pipe_[0] >= 0) ::close(notify_pipe_[0]);
    if (notify_pipe_[1] >= 0) ::close(notify_pipe_[1]);
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
                "Recovering RUNNING job " + job.job_id + " → PENDING (orphaned)");
            job.status = JobStatus::PENDING;
            if (auto r = storage_->update_status(job.job_id, JobStatus::PENDING); r.err()) {
                Logger::error(kComp, "Failed to reset job " + job.job_id + ": " + r.error());
            }
            // Phase 3: clear any stale lease from the previous run.
            static_cast<void>(storage_->clear_lease(job.job_id));
        }
        queue_.push(std::move(job));
        ++recovered;
    }

    Logger::info(kComp, "Recovered " + std::to_string(recovered) + " job(s) from storage");

    // Phase 3: start the lease manager.
    // The callback runs on the LeaseManager's background thread.
    lease_mgr_.start([this](const std::string& job_id, const std::string& worker_id) {
        on_lease_expired(job_id, worker_id);
    });

    return VoidResult::Ok();
}

void JobManager::shutdown() {
    lease_mgr_.stop();
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
#if METRICS_ENABLED
    MetricsRegistry::instance().gauge("job_queue_depth", "PENDING jobs in queue").increment();
#endif
    if (auto r = storage_->persist_job(job); r.err()) {
        // Undo gauge increment on failure.
#if METRICS_ENABLED
        MetricsRegistry::instance().gauge("job_queue_depth", "PENDING jobs in queue").decrement();
#endif
        return Result<std::string>::Err("submit_job storage error: " + r.error());
    }

    const std::string id = job.job_id;

    // Phase 4: record submission timestamp for latency tracking.
#if METRICS_ENABLED
    MetricsRegistry::instance().counter("jobs_submitted_total", "Total jobs submitted").increment();
    LatencyTracker::instance().on_submitted(id, ts);
#endif

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
        if (auto r = storage_->update_status(job->job_id, JobStatus::RUNNING); r.err())
            Logger::warn(kComp, "Failed to mark job RUNNING: " + r.error());
        // Phase 4: record dispatch timestamp.
#if METRICS_ENABLED
        LatencyTracker::instance().on_dispatched(job->job_id, now_ms());
        MetricsRegistry::instance().gauge("job_queue_depth", "PENDING jobs in queue").decrement();
#endif
    }
    return job;
}

std::optional<Job> JobManager::wait_for_job(const StopToken& stop_tok) {
    auto job = queue_.wait_and_pop(stop_tok);
    if (job) {
        if (auto r = storage_->update_status(job->job_id, JobStatus::RUNNING); r.err())
            Logger::warn(kComp, "Failed to mark job RUNNING: " + r.error());
    }
    return job;
}

// ─── complete_job ─────────────────────────────────────────────────────────────

VoidResult JobManager::complete_job(const std::string& job_id) {
    if (auto r = storage_->update_status(job_id, JobStatus::COMPLETED); r.err())
        return VoidResult::Err("complete_job: " + r.error());
    ++jobs_completed_;
    // Phase 4: record completion for latency histograms + counter.
#if METRICS_ENABLED
    MetricsRegistry::instance().counter("jobs_completed_total", "Total jobs completed").increment();
    LatencyTracker::instance().on_completed(job_id, now_ms());
#endif
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
        Logger::warn(kComp,
            "Job " + job_id + " failed (attempt " +
            std::to_string(current_retry_count + 1) + "/" +
            std::to_string(max_retries) + "): " + error + " → re-queuing");

        if (auto r = storage_->increment_retry(job_id); r.err())
            return VoidResult::Err("fail_job increment_retry: " + r.error());

        auto job_result = storage_->get_job(job_id);
        if (job_result.err())
            return VoidResult::Err("fail_job get_job: " + job_result.error());
        if (!job_result.value().has_value())
            return VoidResult::Err("fail_job: job not found after retry: " + job_id);

        queue_.push(std::move(*job_result.value()));
        ++jobs_retried_;
#if METRICS_ENABLED
        MetricsRegistry::instance().counter("jobs_retried_total", "Total jobs retried").increment();
        MetricsRegistry::instance().gauge("job_queue_depth", "PENDING jobs in queue").increment();
#endif

    } else {
        Logger::error(kComp,
            "Job " + job_id + " exceeded max_retries (" +
            std::to_string(max_retries) + ") → FAILED: " + error);

        if (auto r = storage_->update_status(job_id, JobStatus::FAILED, error); r.err())
            return VoidResult::Err("fail_job update_status: " + r.error());
        ++jobs_failed_;
#if METRICS_ENABLED
        MetricsRegistry::instance().counter("jobs_failed_total", "Total jobs permanently failed").increment();
        LatencyTracker::instance().on_failed(job_id, now_ms());
#endif
    }

    return VoidResult::Ok();
}

// ─── requeue_job ─────────────────────────────────────────────────────────────
// Called when a worker disconnects mid-execution OR a lease expires.
// Does NOT consume a retry slot. Clears the lease from storage.

VoidResult JobManager::requeue_job(const std::string& job_id) {
    Logger::warn(kComp,
        "Requeueing job " + job_id + " (lease/disconnect — retry count preserved)");

    // Clear any active lease in storage first.
    static_cast<void>(storage_->clear_lease(job_id));

    if (auto r = storage_->update_status(job_id, JobStatus::PENDING); r.err())
        return VoidResult::Err("requeue_job update_status: " + r.error());

    auto job_result = storage_->get_job(job_id);
    if (job_result.err())
        return VoidResult::Err("requeue_job get_job: " + job_result.error());
    if (!job_result.value().has_value())
        return VoidResult::Err("requeue_job: job not found: " + job_id);

    queue_.push(std::move(*job_result.value()));
#if METRICS_ENABLED
    MetricsRegistry::instance().counter("jobs_requeued_total", "Total jobs requeued on crash/expiry").increment();
    MetricsRegistry::instance().gauge("job_queue_depth", "PENDING jobs in queue").increment();
#endif
    return VoidResult::Ok();
}

// ─── Phase 3: Lease management ───────────────────────────────────────────────

void JobManager::grant_lease(const std::string& job_id,
                              const std::string& worker_id,
                              int64_t duration_ms) {
    // Persist the expiry in storage so crash recovery can detect stale leases.
    int64_t expires_at = now_ms() + duration_ms;
    static_cast<void>(storage_->set_lease(job_id, expires_at));

    // Register with the in-memory LeaseManager for active monitoring.
    lease_mgr_.grant(job_id, worker_id, duration_ms);

    Logger::info(kComp, "Lease granted for job " + job_id +
                          " to worker " + worker_id +
                          " (duration=" + std::to_string(duration_ms / 1000) + "s)");
}

void JobManager::revoke_lease(const std::string& job_id) {
    if (lease_mgr_.revoke(job_id)) {
        static_cast<void>(storage_->clear_lease(job_id));
    }
}

// ─── on_lease_expired ────────────────────────────────────────────────────────
// Called from the LeaseManager background thread when a lease timer fires.
// We MUST NOT call the Server directly from here (wrong thread).
// Instead, requeue_job() is called directly — it is thread-safe (storage
// has its own mutex; queue_ has its own mutex).

void JobManager::on_lease_expired(const std::string& job_id,
                                   const std::string& worker_id) {
    {
        std::lock_guard lk{notify_mu_};
        expired_leases_.push({job_id, worker_id});
    }
    // Wake up the EventLoop
    char c = 'x';
    static_cast<void>(::write(notify_pipe_[1], &c, 1));
}

std::vector<std::pair<std::string, std::string>> JobManager::drain_expired_leases() {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard lk{notify_mu_};
    while (!expired_leases_.empty()) {
        out.push_back(std::move(expired_leases_.front()));
        expired_leases_.pop();
    }
    return out;
}

// ─── Metrics ─────────────────────────────────────────────────────────────────

JobManager::Metrics JobManager::get_metrics() const {
    return Metrics{
        .queue_length   = queue_.size(),
        .jobs_submitted = jobs_submitted_.load(std::memory_order_relaxed),
        .jobs_completed = jobs_completed_.load(std::memory_order_relaxed),
        .jobs_failed    = jobs_failed_.load(std::memory_order_relaxed),
        .jobs_retried   = jobs_retried_.load(std::memory_order_relaxed),
        .active_workers = registry_.size(),
        .active_leases  = lease_mgr_.active_lease_count(),
    };
}
