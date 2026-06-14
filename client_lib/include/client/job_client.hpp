#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// job_client.hpp — Blocking TCP client for job submission
//
// Usage:
//   JobClient client("127.0.0.1", 7777);
//   auto r = client.submit("email_job", R"({"to":"a@b.com"})", Priority::HIGH);
//   if (r.ok()) std::cout << "Job ID: " << r.value() << "\n";
//
// connect() is called lazily on first submit and reconnects automatically
// after disconnect.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>
#include <common/result.hpp>
#include <net/protocol.hpp>

#include <cstdint>
#include <string>

class JobClient {
public:
    JobClient(std::string host, uint16_t port);
    ~JobClient();

    JobClient(const JobClient&)            = delete;
    JobClient& operator=(const JobClient&) = delete;

    // Connect to the server (idempotent — safe to call if already connected).
    [[nodiscard]] VoidResult connect();

    // Submit a job. Blocks until the server acknowledges.
    // Returns the assigned job_id on success, or an error string.
    [[nodiscard]] Result<std::string> submit(
        const std::string& job_type,
        const std::string& payload    = "",
        Priority           priority   = Priority::NORMAL,
        int                max_retries = 3);

    // Disconnect (called automatically in destructor).
    void disconnect();

    [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

private:
    std::string host_;
    uint16_t    port_;
    int         fd_{-1};

    // Blocking send of raw bytes.
    [[nodiscard]] VoidResult send_all(const void* data, size_t len);

    // Blocking receive of exactly `len` bytes.
    [[nodiscard]] VoidResult recv_all(void* data, size_t len);

    // Send one Message and receive one Message (blocking, framed).
    [[nodiscard]] Result<net::Message> transact(const net::Message& request);
};
