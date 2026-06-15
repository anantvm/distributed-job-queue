// tests/unit/test_lease_manager.cpp
//
// Unit tests for LeaseManager (Phase 3)
// ─────────────────────────────────────────────────────────────────────────────

#include <manager/lease_manager.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

// ─── Basic grant / revoke ─────────────────────────────────────────────────────

TEST(LeaseManager, GrantAndRevoke) {
    LeaseManager lm;
    lm.start([](const std::string&, const std::string&) {});

    lm.grant("job-1", "worker-A", 10'000);
    EXPECT_EQ(lm.active_lease_count(), 1);

    bool revoked = lm.revoke("job-1");
    EXPECT_TRUE(revoked);
    EXPECT_EQ(lm.active_lease_count(), 0);

    lm.stop();
}

TEST(LeaseManager, RevokeUnknownJobReturnsFalse) {
    LeaseManager lm;
    lm.start([](const std::string&, const std::string&) {});
    EXPECT_FALSE(lm.revoke("nonexistent"));
    lm.stop();
}

TEST(LeaseManager, MultipleLeases) {
    LeaseManager lm;
    lm.start([](const std::string&, const std::string&) {});

    for (int i = 0; i < 10; ++i)
        lm.grant("job-" + std::to_string(i), "worker", 60'000);

    EXPECT_EQ(lm.active_lease_count(), 10);

    for (int i = 0; i < 10; ++i)
        lm.revoke("job-" + std::to_string(i));

    EXPECT_EQ(lm.active_lease_count(), 0);
    lm.stop();
}

// ─── Lease expiry ────────────────────────────────────────────────────────────

TEST(LeaseManager, ExpiredLeaseFiresCallback) {
    std::atomic<int>    fired{0};
    std::string         fired_job;
    std::string         fired_worker;
    std::mutex          cb_mu;

    LeaseManager lm;
    lm.start([&](const std::string& job_id, const std::string& wid) {
        std::lock_guard lk{cb_mu};
        ++fired;
        fired_job    = job_id;
        fired_worker = wid;
    });

    // Grant a very short lease (50ms) to ensure it fires quickly.
    lm.grant("job-X", "worker-B", 50);

    // Wait up to 3 seconds for the callback (checker runs every 1s).
    for (int i = 0; i < 30 && fired == 0; ++i)
        std::this_thread::sleep_for(100ms);

    EXPECT_EQ(fired.load(), 1);
    {
        std::lock_guard lk{cb_mu};
        EXPECT_EQ(fired_job,    "job-X");
        EXPECT_EQ(fired_worker, "worker-B");
    }
    // Lease should be removed after firing.
    EXPECT_EQ(lm.active_lease_count(), 0);

    lm.stop();
}

TEST(LeaseManager, RevokedLeaseDoesNotFire) {
    std::atomic<int> fired{0};

    LeaseManager lm;
    lm.start([&](const std::string&, const std::string&) { ++fired; });

    lm.grant("job-Y", "worker-C", 50);  // short lease
    lm.revoke("job-Y");                  // cancel before expiry

    // Wait 2s — callback should NOT fire.
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(fired.load(), 0);

    lm.stop();
}

TEST(LeaseManager, MultipleLeasesSomeExpireSomeDont) {
    std::atomic<int> fired{0};

    LeaseManager lm;
    lm.start([&](const std::string&, const std::string&) { ++fired; });

    lm.grant("short", "w", 50);         // will expire
    lm.grant("long",  "w", 60'000);     // will NOT expire in this test

    for (int i = 0; i < 30 && fired < 1; ++i)
        std::this_thread::sleep_for(100ms);

    EXPECT_EQ(fired.load(), 1);
    EXPECT_EQ(lm.active_lease_count(), 1);  // "long" still active

    lm.revoke("long");
    lm.stop();
}

// ─── Re-grant ────────────────────────────────────────────────────────────────

TEST(LeaseManager, ReGrantReplacesExistingLease) {
    std::atomic<int> fired{0};

    LeaseManager lm;
    lm.start([&](const std::string&, const std::string&) { ++fired; });

    lm.grant("job-Z", "w", 50);          // short
    lm.grant("job-Z", "w", 60'000);      // re-grant with long duration → replaces

    // Wait 1.5s — callback should NOT fire because long lease replaced short.
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(fired.load(), 0);

    lm.revoke("job-Z");
    lm.stop();
}

// ─── Stop is prompt ──────────────────────────────────────────────────────────

TEST(LeaseManager, StopExitsQuickly) {
    LeaseManager lm;
    lm.start([](const std::string&, const std::string&) {});

    lm.grant("job-A", "w", 60'000);

    auto t0 = std::chrono::steady_clock::now();
    lm.stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // stop() should return in well under 2 seconds (checker sleep is 1s).
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}

// ─── List leases ─────────────────────────────────────────────────────────────

TEST(LeaseManager, ListLeasesReturnsSnapshot) {
    LeaseManager lm;
    lm.start([](const std::string&, const std::string&) {});

    lm.grant("j1", "w1", 10'000);
    lm.grant("j2", "w2", 10'000);

    auto leases = lm.list_leases();
    EXPECT_EQ(leases.size(), 2u);

    lm.stop();
}
