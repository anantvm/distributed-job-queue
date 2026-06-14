// tests/unit/test_protocol.cpp
//
// Tests for net::serialize / try_deserialize and all factory functions.
// No network required — purely in-process.

#include <net/protocol.hpp>
#include <net/json_util.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace net;

// ─── Round-trip helper ────────────────────────────────────────────────────────

static Message round_trip(const Message& msg) {
    auto bytes = serialize(msg);
    Message out;
    size_t consumed = 0;
    EXPECT_TRUE(try_deserialize(bytes.data(), bytes.size(), out, consumed));
    EXPECT_EQ(consumed, bytes.size());
    return out;
}

// ─── Header encoding ─────────────────────────────────────────────────────────

TEST(Protocol, SerializeHas8ByteHeader) {
    auto bytes = serialize({MsgType::ACK, "{}"});
    ASSERT_GE(bytes.size(), 8u);
    // Big-endian type = 0x0000000A
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x0A);
}

TEST(Protocol, SerializePayloadLength) {
    std::string payload = R"({"job_id":"abc"})";
    auto bytes = serialize({MsgType::COMPLETE_JOB, payload});
    // bytes[4..7] = big-endian length of payload
    uint32_t len_be{};
    std::memcpy(&len_be, bytes.data() + 4, 4);
    uint32_t len = ntohl(len_be);
    EXPECT_EQ(len, payload.size());
}

// ─── Partial data → returns false ────────────────────────────────────────────

TEST(Protocol, PartialHeaderReturnsFalse) {
    auto bytes = serialize({MsgType::ACK, "{}"});
    Message out;
    size_t consumed = 0;
    // Only 4 bytes — not enough for header
    EXPECT_FALSE(try_deserialize(bytes.data(), 4, out, consumed));
    EXPECT_EQ(consumed, 0u);
}

TEST(Protocol, PartialBodyReturnsFalse) {
    std::string payload(100, 'x');
    auto bytes = serialize({MsgType::ACK, payload});
    Message out;
    size_t consumed = 0;
    // Only header + 1 byte — body incomplete
    EXPECT_FALSE(try_deserialize(bytes.data(), 9, out, consumed));
}

// ─── Multiple frames in one buffer ───────────────────────────────────────────

TEST(Protocol, TwoFramesConcatenated) {
    auto b1 = serialize({MsgType::ACK, R"({"ok":true})"});
    auto b2 = serialize({MsgType::NO_JOB, "{}"});
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), b1.begin(), b1.end());
    buf.insert(buf.end(), b2.begin(), b2.end());

    Message out1;
    size_t c1 = 0;
    ASSERT_TRUE(try_deserialize(buf.data(), buf.size(), out1, c1));
    EXPECT_EQ(out1.type, MsgType::ACK);
    EXPECT_EQ(c1, b1.size());

    Message out2;
    size_t c2 = 0;
    ASSERT_TRUE(try_deserialize(buf.data() + c1, buf.size() - c1, out2, c2));
    EXPECT_EQ(out2.type, MsgType::NO_JOB);
}

// ─── Factory / accessor round-trips ──────────────────────────────────────────

TEST(Protocol, SubmitJobRoundTrip) {
    auto msg = round_trip(make_submit_job("email", R"({"to":"a@b.com"})",
                                          Priority::HIGH, 5));
    EXPECT_EQ(msg.type, MsgType::SUBMIT_JOB);
    EXPECT_EQ(json_util::get_string(msg.payload_json, "job_type"), "email");
    EXPECT_EQ(json_util::get_string(msg.payload_json, "priority"), "HIGH");
    EXPECT_EQ(json_util::get_int   (msg.payload_json, "max_retries"), 5);
}

TEST(Protocol, JobSubmittedOk) {
    auto msg = round_trip(make_job_submitted("uuid-1234"));
    EXPECT_EQ(msg.type, MsgType::JOB_SUBMITTED);
    EXPECT_EQ(parse_job_id(msg), "uuid-1234");
    EXPECT_TRUE(parse_ok(msg));
}

TEST(Protocol, JobSubmittedErr) {
    auto msg = round_trip(make_job_submitted_err("storage failed"));
    EXPECT_EQ(msg.type, MsgType::JOB_SUBMITTED);
    EXPECT_FALSE(parse_ok(msg));
    EXPECT_EQ(parse_error(msg), "storage failed");
}

TEST(Protocol, RegisterWorkerRoundTrip) {
    auto msg = round_trip(make_register_worker("worker-abc"));
    EXPECT_EQ(msg.type, MsgType::REGISTER_WORKER);
    EXPECT_EQ(parse_worker_id(msg), "worker-abc");
}

TEST(Protocol, PullJobRoundTrip) {
    auto msg = round_trip(make_pull_job("worker-xyz"));
    EXPECT_EQ(msg.type, MsgType::PULL_JOB);
    EXPECT_EQ(parse_worker_id(msg), "worker-xyz");
}

TEST(Protocol, JobDispatchRoundTrip) {
    Job job;
    job.job_id        = "jid-001";
    job.job_type      = "fibonacci_job";
    job.payload       = "42";
    job.priority      = Priority::HIGH;
    job.status        = JobStatus::RUNNING;
    job.created_at_ms = 1700000000000LL;
    job.updated_at_ms = 1700000001000LL;
    job.max_retries   = 3;
    job.retry_count   = 1;
    job.last_error    = "previous error";

    auto msg  = round_trip(make_job_dispatch(job));
    EXPECT_EQ(msg.type, MsgType::JOB_DISPATCH);
    Job parsed = parse_job(msg);
    EXPECT_EQ(parsed.job_id,        job.job_id);
    EXPECT_EQ(parsed.job_type,      job.job_type);
    EXPECT_EQ(parsed.payload,       job.payload);
    EXPECT_EQ(parsed.priority,      Priority::HIGH);
    EXPECT_EQ(parsed.status,        JobStatus::RUNNING);
    EXPECT_EQ(parsed.created_at_ms, job.created_at_ms);
    EXPECT_EQ(parsed.max_retries,   job.max_retries);
    EXPECT_EQ(parsed.retry_count,   job.retry_count);
    EXPECT_EQ(parsed.last_error,    "previous error");
}

TEST(Protocol, FailJobRoundTrip) {
    auto msg = round_trip(make_fail_job("jid-002", "timeout", 1, 3));
    EXPECT_EQ(msg.type, MsgType::FAIL_JOB);
    EXPECT_EQ(parse_job_id     (msg), "jid-002");
    EXPECT_EQ(parse_error      (msg), "timeout");
    EXPECT_EQ(parse_retry_count(msg), 1);
    EXPECT_EQ(parse_max_retries(msg), 3);
}

TEST(Protocol, HeartbeatRoundTrip) {
    auto msg = round_trip(make_heartbeat("worker-hb"));
    EXPECT_EQ(msg.type, MsgType::HEARTBEAT);
    EXPECT_EQ(parse_worker_id(msg), "worker-hb");
}

TEST(Protocol, EmptyPayloadTypes) {
    for (auto [m, t] : std::vector<std::pair<Message, MsgType>>{
            {make_no_job(),          MsgType::NO_JOB         },
            {make_heartbeat_ack(),   MsgType::HEARTBEAT_ACK  },
            {make_server_shutdown(), MsgType::SERVER_SHUTDOWN },
    }) {
        auto rt = round_trip(m);
        EXPECT_EQ(rt.type, t);
    }
}

TEST(Protocol, PayloadWithSpecialChars) {
    // Ensure escape/unescape of quotes and backslashes works.
    std::string tricky = R"(say "hello" \world\)";
    auto msg = round_trip(make_fail_job("j", tricky, 0, 1));
    EXPECT_EQ(parse_error(msg), tricky);
}
