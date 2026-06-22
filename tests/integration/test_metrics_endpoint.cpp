// tests/integration/test_metrics_endpoint.cpp
//
// Integration tests for the Phase 4 HTTP metrics endpoint.
// Starts the HttpMetricsServer, submits some metrics, polls /metrics,
// verifies the Prometheus text output.

#include <metrics/metrics_registry.hpp>
#include <metrics/http_metrics_server.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

static constexpr uint16_t kMetricsPort = 17780;  // unique test port

// ─── HTTP helper: GET /metrics from localhost:port ────────────────────────────

static std::string http_get_metrics(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    const char* req = "GET /metrics HTTP/1.0\r\nHost: localhost\r\n\r\n";
    ::send(fd, req, ::strlen(req), 0);

    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        response.append(buf, static_cast<size_t>(n));
    ::close(fd);

    // Extract body after \r\n\r\n
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos) return response;
    return response.substr(pos + 4);
}

// ─── Test fixture ─────────────────────────────────────────────────────────────

class MetricsEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().reset_all();
        server_ = std::make_unique<HttpMetricsServer>(kMetricsPort);
        server_->start();
        // Give the server thread time to bind and start accepting
        std::this_thread::sleep_for(50ms);
    }

    void TearDown() override {
        server_->stop();
        server_.reset();
    }

    std::unique_ptr<HttpMetricsServer> server_;
};

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_F(MetricsEndpointTest, ReturnsHTTP200WithBody) {
    MetricsRegistry::instance().counter("dummy", "dummy").increment();
    std::string body = http_get_metrics(kMetricsPort);
    EXPECT_FALSE(body.empty()) << "Expected non-empty metrics body";
}

TEST_F(MetricsEndpointTest, CounterAppearsInOutput) {
    MetricsRegistry::instance().counter("endpoint_test_counter_total", "Test counter").increment(42);
    std::string body = http_get_metrics(kMetricsPort);
    EXPECT_NE(body.find("endpoint_test_counter_total"), std::string::npos);
    EXPECT_NE(body.find("42"), std::string::npos);
}

TEST_F(MetricsEndpointTest, GaugeAppearsInOutput) {
    MetricsRegistry::instance().gauge("endpoint_test_gauge", "Test gauge").set(99);
    std::string body = http_get_metrics(kMetricsPort);
    EXPECT_NE(body.find("endpoint_test_gauge"), std::string::npos);
    EXPECT_NE(body.find("99"), std::string::npos);
}

TEST_F(MetricsEndpointTest, HistogramBucketsAppearsInOutput) {
    MetricsRegistry::instance().histogram("endpoint_test_latency_ms", "Test histogram").observe(25.0);
    std::string body = http_get_metrics(kMetricsPort);
    EXPECT_NE(body.find("endpoint_test_latency_ms_bucket"), std::string::npos);
    EXPECT_NE(body.find("endpoint_test_latency_ms_sum"), std::string::npos);
    EXPECT_NE(body.find("endpoint_test_latency_ms_count"), std::string::npos);
    EXPECT_NE(body.find("+Inf"), std::string::npos);
}

TEST_F(MetricsEndpointTest, HelpAndTypeHeadersPresent) {
    MetricsRegistry::instance().counter("sample_counter_total", "A sample counter").increment(1);
    std::string body = http_get_metrics(kMetricsPort);
    EXPECT_NE(body.find("# HELP"), std::string::npos);
    EXPECT_NE(body.find("# TYPE"), std::string::npos);
}

TEST_F(MetricsEndpointTest, MultiplePolls) {
    // Verify the server handles multiple consecutive requests.
    auto& c = MetricsRegistry::instance().counter("multi_poll_counter_total", "Multi poll");
    for (int i = 1; i <= 5; ++i) {
        c.increment();
        std::string body = http_get_metrics(kMetricsPort);
        EXPECT_NE(body.find("multi_poll_counter_total"), std::string::npos);
    }
}

TEST_F(MetricsEndpointTest, LiveUpdate) {
    auto& c = MetricsRegistry::instance().counter("live_counter_total", "Live update test");
    c.increment(10);
    std::string body1 = http_get_metrics(kMetricsPort);
    EXPECT_NE(body1.find("10"), std::string::npos);

    c.increment(5);
    std::string body2 = http_get_metrics(kMetricsPort);
    EXPECT_NE(body2.find("15"), std::string::npos);
}
