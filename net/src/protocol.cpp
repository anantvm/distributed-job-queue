// net/src/protocol.cpp
#include <net/protocol.hpp>
#include <net/json_util.hpp>

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace net {

// ─── Wire encode/decode ───────────────────────────────────────────────────────

// Header layout: [type:uint32_be][length:uint32_be]
static constexpr size_t kHeaderSize = 8;

std::vector<uint8_t> serialize(const Message& msg) {
    const std::string& body = msg.payload_json;
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + body.size());

    // Big-endian type
    uint32_t type_be = htonl(static_cast<uint32_t>(msg.type));
    const uint8_t* tp = reinterpret_cast<const uint8_t*>(&type_be);
    out.insert(out.end(), tp, tp + 4);

    // Big-endian payload length
    uint32_t len_be  = htonl(static_cast<uint32_t>(body.size()));
    const uint8_t* lp = reinterpret_cast<const uint8_t*>(&len_be);
    out.insert(out.end(), lp, lp + 4);

    // Payload bytes
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(body.data()),
               reinterpret_cast<const uint8_t*>(body.data()) + body.size());

    return out;
}

bool try_deserialize(const uint8_t* data, size_t len,
                     Message& out, size_t& consumed) {
    if (len < kHeaderSize) return false;

    uint32_t type_be{};
    uint32_t plen_be{};
    std::memcpy(&type_be, data,     4);
    std::memcpy(&plen_be, data + 4, 4);

    uint32_t plen = ntohl(plen_be);
    if (len < kHeaderSize + plen) return false;  // body not yet arrived

    out.type         = static_cast<MsgType>(ntohl(type_be));
    out.payload_json = std::string(
        reinterpret_cast<const char*>(data + kHeaderSize), plen);
    consumed = kHeaderSize + plen;
    return true;
}

// ─── Factory functions ────────────────────────────────────────────────────────

Message make_submit_job(const std::string& job_type,
                        const std::string& payload,
                        Priority           priority,
                        int                max_retries) {
    return {MsgType::SUBMIT_JOB,
            json_util::JsonBuilder{}
                .field("job_type",    job_type)
                .field("payload",     payload)
                .field("priority",    to_string(priority))
                .field("max_retries", max_retries)
                .build()};
}

Message make_job_submitted(const std::string& job_id) {
    return {MsgType::JOB_SUBMITTED,
            json_util::JsonBuilder{}.field("job_id", job_id).field("ok", true).build()};
}

Message make_job_submitted_err(const std::string& error) {
    return {MsgType::JOB_SUBMITTED,
            json_util::JsonBuilder{}.field("ok", false).field("error", error).build()};
}

Message make_register_worker(const std::string& worker_id) {
    return {MsgType::REGISTER_WORKER,
            json_util::JsonBuilder{}.field("worker_id", worker_id).build()};
}

Message make_register_ack(bool ok) {
    return {MsgType::REGISTER_ACK,
            json_util::JsonBuilder{}.field("ok", ok).build()};
}

Message make_pull_job(const std::string& worker_id) {
    return {MsgType::PULL_JOB,
            json_util::JsonBuilder{}.field("worker_id", worker_id).build()};
}

Message make_job_dispatch(const Job& job) {
    return {MsgType::JOB_DISPATCH,
            json_util::JsonBuilder{}
                .field("job_id",        job.job_id)
                .field("job_type",      job.job_type)
                .field("payload",       job.payload)
                .field("priority",      to_string(job.priority))
                .field("status",        to_string(job.status))
                .field("created_at_ms", job.created_at_ms)
                .field("updated_at_ms", job.updated_at_ms)
                .field("max_retries",   job.max_retries)
                .field("retry_count",   job.retry_count)
                .field("last_error",    job.last_error)
                .build()};
}

Message make_no_job() {
    return {MsgType::NO_JOB, "{}"};
}

Message make_complete_job(const std::string& job_id) {
    return {MsgType::COMPLETE_JOB,
            json_util::JsonBuilder{}.field("job_id", job_id).build()};
}

Message make_fail_job(const std::string& job_id,
                      const std::string& error,
                      int retry_count,
                      int max_retries) {
    return {MsgType::FAIL_JOB,
            json_util::JsonBuilder{}
                .field("job_id",       job_id)
                .field("error",        error)
                .field("retry_count",  retry_count)
                .field("max_retries",  max_retries)
                .build()};
}

Message make_ack(bool ok, const std::string& error) {
    return {MsgType::ACK,
            json_util::JsonBuilder{}.field("ok", ok).field("error", error).build()};
}

Message make_heartbeat(const std::string& worker_id) {
    return {MsgType::HEARTBEAT,
            json_util::JsonBuilder{}.field("worker_id", worker_id).build()};
}

Message make_heartbeat_ack() { return {MsgType::HEARTBEAT_ACK, "{}"}; }
Message make_server_shutdown() { return {MsgType::SERVER_SHUTDOWN, "{}"}; }

// ─── Payload accessors ────────────────────────────────────────────────────────

std::string parse_job_id     (const Message& m) { return json_util::get_string(m.payload_json, "job_id"); }
std::string parse_error      (const Message& m) { return json_util::get_string(m.payload_json, "error"); }
bool        parse_ok         (const Message& m) { return json_util::get_bool  (m.payload_json, "ok"); }
std::string parse_worker_id  (const Message& m) { return json_util::get_string(m.payload_json, "worker_id"); }
int         parse_retry_count(const Message& m) { return json_util::get_int   (m.payload_json, "retry_count"); }
int         parse_max_retries(const Message& m) { return json_util::get_int   (m.payload_json, "max_retries"); }

Job parse_job(const Message& m) {
    const std::string& j = m.payload_json;
    Job job;
    job.job_id        = json_util::get_string(j, "job_id");
    job.job_type      = json_util::get_string(j, "job_type");
    job.payload       = json_util::get_string(j, "payload");
    job.priority      = priority_from_string(json_util::get_string(j, "priority"));
    job.status        = status_from_string  (json_util::get_string(j, "status"));
    job.created_at_ms = json_util::get_int64(j, "created_at_ms");
    job.updated_at_ms = json_util::get_int64(j, "updated_at_ms");
    job.max_retries   = json_util::get_int  (j, "max_retries");
    job.retry_count   = json_util::get_int  (j, "retry_count");
    job.last_error    = json_util::get_string(j, "last_error");
    return job;
}

} // namespace net
