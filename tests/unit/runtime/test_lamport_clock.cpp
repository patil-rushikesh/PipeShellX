#include "psx/runtime/lamport_clock.hpp"

#include <gtest/gtest.h>

using psx::runtime::LamportClock;

TEST(LamportClockTest, StartsAtZero) {
    LamportClock clock;
    EXPECT_EQ(clock.value(), 0U);
}

TEST(LamportClockTest, TickIncrementsByOneAndReturnsNewValue) {
    LamportClock clock;
    EXPECT_EQ(clock.tick(), 1U);
    EXPECT_EQ(clock.tick(), 2U);
    EXPECT_EQ(clock.tick(), 3U);
    EXPECT_EQ(clock.value(), 3U);
}

TEST(LamportClockTest, ObserveTakesMaxOfLocalAndReceivedThenAddsOne) {
    LamportClock clock; // local = 0
    // received (10) > local (0): new value is max(0,10)+1 = 11
    EXPECT_EQ(clock.observe(10), 11U);

    clock.tick(); // local = 12
    clock.tick(); // local = 13
    // received (5) <= local (13): new value is max(13,5)+1 = 14
    EXPECT_EQ(clock.observe(5), 14U);
}

TEST(LamportClockTest, ValueHasNoSideEffect) {
    LamportClock clock;
    clock.tick();
    const auto before = clock.value();
    const auto again = clock.value();
    EXPECT_EQ(before, again);
    EXPECT_EQ(clock.value(), 1U);
}

TEST(LamportClockTest, ExplicitInitialValueIsHonoured) {
    LamportClock clock(100);
    EXPECT_EQ(clock.value(), 100U);
    EXPECT_EQ(clock.tick(), 101U);
}

// Classic Lamport property: if A happens-before B because A's message
// carried A's timestamp to the process that then does B, timestamp(A) must
// be strictly less than timestamp(B).
TEST(LamportClockTest, MessagePassingPreservesHappensBefore) {
    LamportClock sender;
    LamportClock receiver;

    const auto tsA = sender.tick(); // event A: sender ticks before sending
    const auto tsB = receiver.observe(tsA); // event B: receiver processes the message

    EXPECT_LT(tsA, tsB);

    // A later local event on the receiver must still be strictly after B.
    const auto tsC = receiver.tick();
    EXPECT_LT(tsB, tsC);
}

// Security regression: OPEN v2 carries an attacker-chosen u64. A peer that
// sends UINT64_MAX must not be able to wrap the receiver's clock back to 0,
// which would make every later event look older than earlier ones.
TEST(LamportClockTest, ObserveOfMaxValueSaturatesInsteadOfWrapping) {
    LamportClock clock;
    clock.tick();
    const auto v = clock.observe(LamportClock::kMax);
    EXPECT_EQ(v, LamportClock::kMax);
    EXPECT_TRUE(clock.saturated());
    EXPECT_EQ(clock.tick(), LamportClock::kMax); // stays put, never wraps
    EXPECT_EQ(clock.observe(5), LamportClock::kMax);
}

TEST(LamportClockTest, ObserveJustBelowMaxReachesMaxWithoutWrapping) {
    LamportClock clock;
    EXPECT_EQ(clock.observe(LamportClock::kMax - 1), LamportClock::kMax);
    EXPECT_FALSE(clock.value() == 0);
}

TEST(LamportClockTest, TickAtMaxDoesNotWrap) {
    LamportClock clock(LamportClock::kMax);
    EXPECT_EQ(clock.tick(), LamportClock::kMax);
    EXPECT_TRUE(clock.saturated());
}
