#include "psx/sim/simulation.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using psx::sim::ElectionConfig;
using psx::sim::MutexConfig;
using psx::sim::runElection;
using psx::sim::runMutex;

// Property tests over many seeds: random per-message delays mean messages are
// reordered differently every run, so these exercise interleavings no
// hand-written test would think of. Each run goes through the real wire codec.

TEST(ElectionSimTest, ConcurrentBootAlwaysElectsTheHighestId) {
    for (std::uint32_t seed = 1; seed <= 150; ++seed) {
        ElectionConfig c;
        c.nodes = 3 + seed % 5;
        c.network = {.seed = seed, .minDelay = 1ms, .maxDelay = 80ms};
        c.duration = 4000ms;
        c.traceMessages = false;
        const auto r = runElection(c);
        ASSERT_TRUE(r.agreed) << "seed " << seed << ": no agreement";
        EXPECT_EQ(r.expectedLeader, c.nodes) << "seed " << seed;
    }
}

TEST(ElectionSimTest, LeaderCrashIsRecoveredUnderReordering) {
    for (std::uint32_t seed = 1; seed <= 150; ++seed) {
        ElectionConfig c;
        c.nodes = 3 + seed % 5;
        c.network = {.seed = seed, .minDelay = 1ms, .maxDelay = 80ms};
        c.killLeaderAt = 2000ms;
        c.duration = 8000ms;
        c.traceMessages = false;
        const auto r = runElection(c);
        ASSERT_TRUE(r.killedNode.has_value()) << "seed " << seed;
        EXPECT_EQ(*r.killedNode, c.nodes) << "seed " << seed << ": the leader was not the highest id";
        ASSERT_TRUE(r.agreed) << "seed " << seed << ": survivors never agreed";
        EXPECT_EQ(r.expectedLeader, c.nodes - 1) << "seed " << seed;
        ASSERT_TRUE(r.recoveredAtMs.has_value());
        // Bound: leader silence (leaderTimeout) + election (answer timeout) +
        // a few network hops. Generous, but finite and checked.
        EXPECT_LT(*r.recoveredAtMs - 2000, 3000) << "seed " << seed << ": recovery too slow";
    }
}

TEST(ElectionSimTest, RevivedHighestNodeTakesLeadershipBack) {
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        ElectionConfig c;
        c.nodes = 4;
        c.network = {.seed = seed, .minDelay = 1ms, .maxDelay = 60ms};
        c.killLeaderAt = 1500ms;
        c.reviveAt = 4000ms;
        c.duration = 8000ms;
        c.traceMessages = false;
        const auto r = runElection(c);
        ASSERT_TRUE(r.agreed) << "seed " << seed;
        EXPECT_EQ(r.expectedLeader, 4U) << "seed " << seed;
    }
}

TEST(ElectionSimTest, SameSeedGivesTheSameTimeline) {
    ElectionConfig c;
    c.network.seed = 42;
    c.killLeaderAt = 2000ms;
    const auto a = runElection(c);
    const auto b = runElection(c);
    ASSERT_EQ(a.events.size(), b.events.size());
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        EXPECT_EQ(a.events[i].text, b.events[i].text);
        EXPECT_EQ(a.events[i].timeMs, b.events[i].timeMs);
    }
}

TEST(MutexSimTest, RicartAgrawalaIsSafeLiveAndFairUnderReordering) {
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        MutexConfig c;
        c.nodes = 3 + seed % 4;
        c.network = {.seed = seed, .minDelay = 1ms, .maxDelay = 50ms};
        c.requestsPerNode = 3;
        c.traceMessages = false;
        const auto r = runMutex(c);
        ASSERT_EQ(r.violations, 0U) << "seed " << seed << ": two nodes inside together";
        EXPECT_LE(r.maxConcurrentHolders, 1U) << "seed " << seed;
        EXPECT_EQ(r.requestsServed, r.requestsExpected) << "seed " << seed << ": starvation";
        EXPECT_EQ(r.requestsExpected, c.nodes * c.requestsPerNode) << "seed " << seed;
        EXPECT_TRUE(r.grantedInTimestampOrder) << "seed " << seed;
        // Message complexity: exactly 2(N-1) messages per entry.
        EXPECT_EQ(r.messagesSent, static_cast<std::uint64_t>(r.requestsServed) * 2U * (c.nodes - 1)) << "seed " << seed;
    }
}

// The baseline the algorithm exists to prevent: without coordination,
// overlapping critical sections happen.
TEST(MutexSimTest, NaiveBaselineOverlaps) {
    MutexConfig c;
    c.nodes = 3;
    c.naive = true;
    c.network.seed = 7;
    const auto r = runMutex(c);
    EXPECT_GT(r.violations, 0U);
    EXPECT_GT(r.maxConcurrentHolders, 1U);
}

TEST(MutexSimTest, CrashedNodeIsRemovedByTheFailureDetectorAndOthersProceed) {
    for (std::uint32_t seed = 1; seed <= 60; ++seed) {
        MutexConfig c;
        c.nodes = 4;
        c.network = {.seed = seed, .minDelay = 1ms, .maxDelay = 40ms};
        c.requestsPerNode = 3;
        c.killNode = 2;
        c.killAt = 30ms; // mid-protocol: requests are in flight
        c.detectAfter = 400ms;
        c.traceMessages = false;
        const auto r = runMutex(c);
        EXPECT_EQ(r.violations, 0U) << "seed " << seed;
        EXPECT_EQ(r.requestsServed, r.requestsExpected) << "seed " << seed << ": survivors stayed blocked";
    }
}
