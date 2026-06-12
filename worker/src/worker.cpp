// worker/src/worker.cpp
#include <worker/worker.hpp>

#include <common/logger.hpp>
#include <common/uuid.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

// ─── Constructor / Destructor ─────────────────────────────────────────────────

Worker::Worker(int id, JobManager& manager, const HandlerRegistry& registry)
    : id_(id)
    , worker_id_(uuid::generate())
    , manager_(manager)
    , registry_(registry) {}

Worker::~Worker() {
    stop();  // safe to call even if thread not started or already stopped
}

// ─── start / stop ────────────────────────────────────────────────────────────

void Worker::start() {
    thread_ = std::thread([this] { run(); });
    Logger::info(component_name(), "Started (worker_id=" + worker_id_ + ")");
}

void Worker::stop() {
    stop_source_.request_stop();
    if (thread_.joinable()) thread_.join();
}

// ─── run — the main event loop ────────────────────────────────────────────────

void Worker::run() {
    running_.store(true);
    Logger::info(component_name(), "Event loop started");

    const StopToken stop_tok = stop_source_.get_token();

    while (!stop_tok.stop_requested()) {
        auto job_opt = manager_.wait_for_job(stop_tok);
        if (!job_opt.has_value()) break;
        execute_job(*job_opt);
    }

    running_.store(false);
    Logger::info(component_name(), "Event loop exited");
}

// ─── execute_job ─────────────────────────────────────────────────────────────

void Worker::execute_job(const Job& job) {
    const std::string comp = component_name();
    Logger::info(comp,
        "Executing job " + job.job_id +
        " type=" + job.job_type +
        " priority=" + to_string(job.priority) +
        " attempt=" + std::to_string(job.retry_count + 1));

    ++jobs_executed_;

    const auto start = std::chrono::steady_clock::now();

    // ── Handler lookup ────────────────────────────────────────────────────────
    auto handler_opt = registry_.find(job.job_type);
    if (!handler_opt.has_value()) {
        const std::string err = "No handler registered for job_type: " + job.job_type;
        Logger::error(comp, err);
        ++jobs_failed_;
        // Force permanent failure by passing max_retries as current count.
        if (auto r = manager_.fail_job(job.job_id, err, job.max_retries, job.max_retries); r.err()) {
            Logger::error(comp, "fail_job error: " + r.error());
        }
        return;
    }

    // ── Handler execution ─────────────────────────────────────────────────────
    try {
        (*handler_opt)(job);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        total_exec_ms_.fetch_add(elapsed.count(), std::memory_order_relaxed);
        ++jobs_succeeded_;

        Logger::info(comp,
            "Completed job " + job.job_id +
            " in " + std::to_string(elapsed.count()) + "ms");

        if (auto r = manager_.complete_job(job.job_id); r.err()) {
            Logger::error(comp, "complete_job error: " + r.error());
        }

    } catch (const std::exception& ex) {
        const std::string err = ex.what();
        Logger::warn(comp, "Job " + job.job_id + " threw: " + err);
        ++jobs_failed_;
        if (auto r = manager_.fail_job(job.job_id, err, job.retry_count, job.max_retries); r.err()) {
            Logger::error(comp, "fail_job error: " + r.error());
        }

    } catch (...) {
        const std::string err = "unknown exception";
        Logger::warn(comp, "Job " + job.job_id + " threw unknown exception");
        ++jobs_failed_;
        if (auto r = manager_.fail_job(job.job_id, err, job.retry_count, job.max_retries); r.err()) {
            Logger::error(comp, "fail_job error: " + r.error());
        }
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string Worker::component_name() const {
    return "Worker-" + std::to_string(id_);
}

Worker::Stats Worker::stats() const {
    return Stats{
        .jobs_executed        = jobs_executed_.load(std::memory_order_relaxed),
        .jobs_succeeded       = jobs_succeeded_.load(std::memory_order_relaxed),
        .jobs_failed          = jobs_failed_.load(std::memory_order_relaxed),
        .total_execution_time = std::chrono::milliseconds{
            total_exec_ms_.load(std::memory_order_relaxed)},
    };
}
