// worker/src/handler_registry.cpp
#include <worker/handler_registry.hpp>

void HandlerRegistry::register_handler(const std::string& job_type, JobHandler handler) {
    handlers_[job_type] = std::move(handler);
}

std::optional<JobHandler> HandlerRegistry::find(const std::string& job_type) const {
    auto it = handlers_.find(job_type);
    if (it == handlers_.end()) return std::nullopt;
    return it->second;
}

bool HandlerRegistry::has_handler(const std::string& job_type) const {
    return handlers_.count(job_type) > 0;
}
