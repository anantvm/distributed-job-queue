#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// handler_registry.hpp
//
// Maps job_type strings to C++ handler functions.
//
// A handler receives a const reference to the job.  It should:
//   • Execute the work described by job.payload.
//   • Return normally on success.
//   • Throw any std::exception subclass on failure — the Worker catches it
//     and calls JobManager::fail_job() on the worker's behalf.
//
// Handlers are registered once at startup; the registry is then read-only
// during execution, so no locking is needed for Find().
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

// A job handler is a plain callable — use lambdas, free functions, or
// std::bind for member functions.
using JobHandler = std::function<void(const Job&)>;

class HandlerRegistry {
public:
    HandlerRegistry() = default;

    // Register a handler for the given job_type.
    // Overwrites any previously registered handler for the same type.
    void register_handler(const std::string& job_type, JobHandler handler);

    // Look up the handler for a job_type.
    // Returns nullopt if no handler was registered.
    [[nodiscard]] std::optional<JobHandler> find(const std::string& job_type) const;

    [[nodiscard]] bool   has_handler(const std::string& job_type) const;
    [[nodiscard]] size_t size() const { return handlers_.size(); }

private:
    std::unordered_map<std::string, JobHandler> handlers_;
};
