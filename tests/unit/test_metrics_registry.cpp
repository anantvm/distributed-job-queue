// tests/unit/test_metrics_registry.cpp
#include <metrics/metrics_registry.hpp>
#include <gtest/gtest.h>

// Use a fresh scope per test group by relying on reset_all()
class MetricsRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetricsRegistry::instance().reset_all();
    }
};

TEST_F(MetricsRegistryTest, CounterRegistrationAndRetrieval) {
    auto& c1 = MetricsRegistry::instance().counter("test_requests_total", "Test counter");
    auto& c2 = MetricsRegistry::instance().counter("test_requests_total");  // same name
    EXPECT_EQ(&c1, &c2);  // same object returned
    c1.increment(5);
    EXPECT_EQ(c2.value(), 5u);
}

TEST_F(MetricsRegistryTest, GaugeRegistrationAndRetrieval) {
    auto& g1 = MetricsRegistry::instance().gauge("test_queue_depth", "Test gauge");
    auto& g2 = MetricsRegistry::instance().gauge("test_queue_depth");
    EXPECT_EQ(&g1, &g2);
    g1.set(42);
    EXPECT_EQ(g2.value(), 42);
}

TEST_F(MetricsRegistryTest, HistogramRegistrationAndRetrieval) {
    auto& h1 = MetricsRegistry::instance().histogram("test_latency_ms", "Test histogram");
    auto& h2 = MetricsRegistry::instance().histogram("test_latency_ms");
    EXPECT_EQ(&h1, &h2);
    h1.observe(50.0);
    auto s = h2.snapshot();
    EXPECT_EQ(s.count, 1u);
}

TEST_F(MetricsRegistryTest, PrometheusTextContainsCounter) {
    MetricsRegistry::instance().counter("http_requests_total", "Total HTTP requests").increment(7);
    std::string text = MetricsRegistry::instance().prometheus_text();
    EXPECT_NE(text.find("http_requests_total"), std::string::npos);
    EXPECT_NE(text.find("7"), std::string::npos);
    EXPECT_NE(text.find("# HELP"), std::string::npos);
    EXPECT_NE(text.find("# TYPE"), std::string::npos);
    EXPECT_NE(text.find("counter"), std::string::npos);
}

TEST_F(MetricsRegistryTest, PrometheusTextContainsGauge) {
    MetricsRegistry::instance().gauge("queue_depth", "Queue depth").set(12);
    std::string text = MetricsRegistry::instance().prometheus_text();
    EXPECT_NE(text.find("queue_depth"), std::string::npos);
    EXPECT_NE(text.find("gauge"), std::string::npos);
    EXPECT_NE(text.find("12"), std::string::npos);
}

TEST_F(MetricsRegistryTest, PrometheusTextContainsHistogramBuckets) {
    auto& h = MetricsRegistry::instance().histogram("latency_ms", "Latency");
    h.observe(8.0);
    std::string text = MetricsRegistry::instance().prometheus_text();
    EXPECT_NE(text.find("latency_ms_bucket"), std::string::npos);
    EXPECT_NE(text.find("latency_ms_sum"), std::string::npos);
    EXPECT_NE(text.find("latency_ms_count"), std::string::npos);
    EXPECT_NE(text.find("+Inf"), std::string::npos);
}

TEST_F(MetricsRegistryTest, ResetAllClearsMetrics) {
    MetricsRegistry::instance().counter("temp_counter").increment(100);
    MetricsRegistry::instance().reset_all();
    // After reset, values should be 0
    EXPECT_EQ(MetricsRegistry::instance().counter("temp_counter").value(), 0);
}

TEST_F(MetricsRegistryTest, MultipleDistinctMetrics) {
    MetricsRegistry::instance().counter("c1").increment(1);
    MetricsRegistry::instance().counter("c2").increment(2);
    MetricsRegistry::instance().gauge("g1").set(10);
    std::string text = MetricsRegistry::instance().prometheus_text();
    EXPECT_NE(text.find("c1"), std::string::npos);
    EXPECT_NE(text.find("c2"), std::string::npos);
    EXPECT_NE(text.find("g1"), std::string::npos);
}
