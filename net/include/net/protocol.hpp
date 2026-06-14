#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// protocol.hpp — Wire Protocol for the Distributed Job Queue
//
// Frame format (all integers big-endian):
//
//   ┌──────────┬──────────┬─────────────────────────┐
//   │ type     │ length   │ payload (JSON UTF-8)     │
//   │ uint32   │ uint32   │ <length> bytes           │
//   └──────────┴──────────┴─────────────────────────┘
//
// Message types and JSON payload schemas are documented per enum value.
// ─────────────────────────────────────────────────────────────────────────────

#include <common/job.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace net {

// ─── Message Type ─────────────────────────────────────────────────────────────

enum class MsgType : uint32_t {
    SUBMIT_JOB       = 0x01,  // Client→Server: {job_type, payload, priority, max_retries}
    JOB_SUBMITTED    = 0x02,  // Server→Client: {job_id} or {error}
    REGISTER_WORKER  = 0x03,  // Worker→Server: {worker_id}
    REGISTER_ACK     = 0x04,  // Server→Worker: {ok}
    PULL_JOB         = 0x05,  // Worker→Server: {worker_id}
    JOB_DISPATCH     = 0x06,  // Server→Worker: {job_id, job_type, payload, priority, status,
                              //                  created_at_ms, updated_at_ms, max_retries,
                              //                  retry_count, last_error}
    NO_JOB           = 0x07,  // Server→Worker: {}  (queue was empty)
    COMPLETE_JOB     = 0x08,  // Worker→Server: {job_id}
    FAIL_JOB         = 0x09,  // Worker→Server: {job_id, error, retry_count, max_retries}
    ACK              = 0x0A,  // Server→Worker: {ok, error}
    HEARTBEAT        = 0x0B,  // Worker→Server: {worker_id}
    HEARTBEAT_ACK    = 0x0C,  // Server→Worker: {}
    SERVER_SHUTDOWN  = 0x0D,  // Server→All:    {}
};

// ─── Message ─────────────────────────────────────────────────────────────────

struct Message {
    MsgType     type{MsgType::ACK};
    std::string payload_json;
};

// ─── Serialisation ───────────────────────────────────────────────────────────

// Encode a Message to wire bytes (8-byte header + JSON body).
[[nodiscard]] std::vector<uint8_t> serialize(const Message& msg);

// Try to decode one Message from [data, data+len).
// Returns true and sets `out` and `consumed` on success.
// Returns false (no modification) if data is incomplete.
[[nodiscard]] bool try_deserialize(const uint8_t* data, size_t len,
                                   Message& out, size_t& consumed);

// ─── Factory functions ────────────────────────────────────────────────────────

[[nodiscard]] Message make_submit_job(const std::string& job_type,
                                      const std::string& payload,
                                      Priority           priority,
                                      int                max_retries);

[[nodiscard]] Message make_job_submitted(const std::string& job_id);
[[nodiscard]] Message make_job_submitted_err(const std::string& error);

[[nodiscard]] Message make_register_worker(const std::string& worker_id);
[[nodiscard]] Message make_register_ack(bool ok);

[[nodiscard]] Message make_pull_job(const std::string& worker_id);
[[nodiscard]] Message make_job_dispatch(const Job& job);
[[nodiscard]] Message make_no_job();

[[nodiscard]] Message make_complete_job(const std::string& job_id);
[[nodiscard]] Message make_fail_job(const std::string& job_id,
                                    const std::string& error,
                                    int retry_count,
                                    int max_retries);

[[nodiscard]] Message make_ack(bool ok, const std::string& error = "");
[[nodiscard]] Message make_heartbeat(const std::string& worker_id);
[[nodiscard]] Message make_heartbeat_ack();
[[nodiscard]] Message make_server_shutdown();

// ─── Payload accessors ────────────────────────────────────────────────────────

[[nodiscard]] std::string parse_job_id     (const Message& m);
[[nodiscard]] std::string parse_error      (const Message& m);
[[nodiscard]] bool        parse_ok         (const Message& m);
[[nodiscard]] std::string parse_worker_id  (const Message& m);
[[nodiscard]] int         parse_retry_count(const Message& m);
[[nodiscard]] int         parse_max_retries(const Message& m);
[[nodiscard]] Job         parse_job        (const Message& m);

} // namespace net
