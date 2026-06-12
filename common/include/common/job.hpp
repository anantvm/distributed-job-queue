#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// job.hpp
//
// Core domain types:
//   Priority   — controls dispatch ordering (HIGH > NORMAL > LOW).
//   JobStatus  — lifecycle state machine for a job.
//   Job        — the fundamental unit of work.
//
// Jobs are value types; they are moved through the system to avoid copies.
// Comparison semantics drive the priority queue ordering.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>

// ─── Priority ─────────────────────────────────────────────────────────────────
enum class Priority : uint8_t {
    LOW    = 0,
    NORMAL = 1,
    HIGH   = 2,
};

inline std::string to_string(Priority p) {
    switch (p) {
        case Priority::LOW:    return "LOW";
        case Priority::NORMAL: return "NORMAL";
        case Priority::HIGH:   return "HIGH";
    }
    return "NORMAL";
}

inline Priority priority_from_string(const std::string& s) {
    if (s == "HIGH") return Priority::HIGH;
    if (s == "LOW")  return Priority::LOW;
    return Priority::NORMAL;
}

inline int priority_value(Priority p) noexcept {
    return static_cast<int>(p);
}

// ─── JobStatus ────────────────────────────────────────────────────────────────
// State machine:
//   PENDING → RUNNING → COMPLETED
//                     ↘ FAILED → (retry → PENDING) | DLQ
enum class JobStatus : uint8_t {
    PENDING   = 0,
    RUNNING   = 1,
    COMPLETED = 2,
    FAILED    = 3,
    DLQ       = 4,   // dead-letter queue (exceeded max_retries)
};

inline std::string to_string(JobStatus s) {
    switch (s) {
        case JobStatus::PENDING:   return "PENDING";
        case JobStatus::RUNNING:   return "RUNNING";
        case JobStatus::COMPLETED: return "COMPLETED";
        case JobStatus::FAILED:    return "FAILED";
        case JobStatus::DLQ:       return "DLQ";
    }
    return "PENDING";
}

inline JobStatus status_from_string(const std::string& s) {
    if (s == "RUNNING")   return JobStatus::RUNNING;
    if (s == "COMPLETED") return JobStatus::COMPLETED;
    if (s == "FAILED")    return JobStatus::FAILED;
    if (s == "DLQ")       return JobStatus::DLQ;
    return JobStatus::PENDING;
}

// ─── Job ──────────────────────────────────────────────────────────────────────
struct Job {
    std::string job_id;            // UUID v4 assigned at submission
    std::string job_type;          // selects the handler in the worker's registry
    std::string payload;           // arbitrary data (JSON string in Phase 1)
    Priority    priority{Priority::NORMAL};
    JobStatus   status{JobStatus::PENDING};
    int64_t     created_at_ms{0};  // Unix epoch milliseconds
    int64_t     updated_at_ms{0};
    int         max_retries{3};
    int         retry_count{0};
    std::string last_error;        // populated on failure

    // ── Ordering for std::priority_queue ─────────────────────────────────────
    // std::priority_queue<T> is a max-heap: top() returns the "greatest" Job.
    // We define "greater" as: higher priority, or (same priority) older job.
    //
    // This operator is used by the custom JobComparator below.
    // A < B means "B should be processed before A"
    bool operator<(const Job& other) const noexcept {
        if (priority != other.priority) {
            // lower enum value = lower priority → this job is "less than" a higher-priority job
            return priority_value(priority) < priority_value(other.priority);
        }
        // same priority: older job (smaller timestamp) is "greater"
        return created_at_ms > other.created_at_ms;
    }
};
