#include "psx/sync/ricart_agrawala.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <tuple>
#include <vector>

using psx::sync::decodeMutexMessage;
using psx::sync::encodeMutexMessage;
using psx::sync::MutexMessage;
using psx::sync::PeerId;
using psx::sync::RicartAgrawala;

namespace {

RicartAgrawala make(PeerId self, std::vector<PeerId> peers) {
    auto ra = RicartAgrawala::create(self, std::move(peers));
    EXPECT_TRUE(ra.ok());
    return std::move(ra.value());
}

// A tiny in-memory network: nodes plus an in-flight message list. Tests pick
// which message is delivered next, so any interleaving can be forced.
struct Cluster {
    std::map<PeerId, RicartAgrawala> nodes;
    std::vector<MutexMessage> inFlight;

    explicit Cluster(const std::vector<PeerId>& ids) {
        for (const PeerId id : ids) {
            std::vector<PeerId> peers;
            for (const PeerId other : ids) {
                if (other != id) {
                    peers.push_back(other);
                }
            }
            nodes.emplace(id, make(id, peers));
        }
    }
    RicartAgrawala& at(PeerId id) { return nodes.at(id); }
    void send(const std::vector<MutexMessage>& out) { inFlight.insert(inFlight.end(), out.begin(), out.end()); }
    void request(PeerId id) {
        auto out = at(id).request();
        ASSERT_TRUE(out.ok()) << out.error().message();
        send(out.value());
    }
    void release(PeerId id) {
        auto out = at(id).release();
        ASSERT_TRUE(out.ok()) << out.error().message();
        send(out.value());
    }
    void deliver(std::size_t index) {
        const MutexMessage m = inFlight[index];
        inFlight.erase(inFlight.begin() + static_cast<std::ptrdiff_t>(index));
        auto out = at(m.to).onMessage(m);
        ASSERT_TRUE(out.ok()) << out.error().message();
        send(out.value());
    }
    void deliverAll() {
        while (!inFlight.empty()) {
            deliver(0);
        }
    }
    std::size_t holders() const {
        return static_cast<std::size_t>(
            std::count_if(nodes.begin(), nodes.end(), [](const auto& kv) { return kv.second.holding(); }));
    }
};

} // namespace

TEST(RicartAgrawalaTest, CreateRejectsSelfAndDuplicatePeers) {
    EXPECT_FALSE(RicartAgrawala::create(1, {1, 2}).ok());
    EXPECT_FALSE(RicartAgrawala::create(1, {2, 2}).ok());
    EXPECT_TRUE(RicartAgrawala::create(1, {2, 3}).ok());
}

TEST(RicartAgrawalaTest, LoneNodeEntersImmediately) {
    auto ra = make(1, {});
    auto out = ra.request();
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().empty());
    EXPECT_TRUE(ra.holding());
}

TEST(RicartAgrawalaTest, UncontendedRequestNeedsEveryReply) {
    Cluster c({1, 2, 3});
    c.request(1);
    ASSERT_EQ(c.inFlight.size(), 2U); // REQUEST to 2 and 3
    c.deliver(0);                     // 2 receives and replies
    c.deliver(0);                     // 3 receives and replies
    ASSERT_EQ(c.inFlight.size(), 2U); // two REPLYs
    c.deliver(0);
    EXPECT_FALSE(c.at(1).holding()) << "entered with one reply outstanding";
    c.deliver(0);
    EXPECT_TRUE(c.at(1).holding());
}

TEST(RicartAgrawalaTest, HolderDefersAndReleaseHandsOver) {
    Cluster c({1, 2});
    c.request(1);
    c.deliverAll();
    ASSERT_TRUE(c.at(1).holding());
    c.request(2);
    c.deliverAll();
    EXPECT_FALSE(c.at(2).holding());
    EXPECT_EQ(c.at(1).deferredReplies(), 1U);
    c.release(1);
    c.deliverAll();
    EXPECT_TRUE(c.at(2).holding());
    EXPECT_EQ(c.holders(), 1U);
}

// Equal Lamport timestamps: the lower id wins; the other is served next.
TEST(RicartAgrawalaTest, SimultaneousRequestsTieBreakOnId) {
    Cluster c({1, 2});
    c.request(1);
    c.request(2);
    EXPECT_EQ(c.at(1).requestTimestamp(), c.at(2).requestTimestamp());
    c.deliverAll();
    EXPECT_TRUE(c.at(1).holding());
    EXPECT_FALSE(c.at(2).holding());
    c.release(1);
    c.deliverAll();
    EXPECT_TRUE(c.at(2).holding());
}

// An older request wins even against a lower id (timestamp order, not id order).
TEST(RicartAgrawalaTest, OlderTimestampWinsRegardlessOfId) {
    Cluster c({1, 2});
    c.request(2); // node 2 asks first: ts = 1
    c.deliver(0); // node 1 sees it (clock -> 2) and replies (clock -> 3)
    c.request(1); // node 1 asks later: ts = 4
    ASSERT_LT(c.at(2).requestTimestamp(), c.at(1).requestTimestamp());
    c.deliverAll();
    EXPECT_TRUE(c.at(2).holding()) << "higher id with the older request must win";
    EXPECT_FALSE(c.at(1).holding());
    c.release(2);
    c.deliverAll();
    EXPECT_TRUE(c.at(1).holding());
}

// Property test: random interleavings must never admit two holders, and every
// request must eventually be granted, in (timestamp, id) order. Holders stay
// in the section for a random number of delivery steps (so requests really do
// arrive while someone holds it), and every node requests twice (so stale
// state from round 1 is exercised in round 2).
TEST(RicartAgrawalaTest, RandomInterleavingsPreserveSafetyLivenessAndFairness) {
    constexpr int kRounds = 2;
    for (unsigned seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng(seed);
        const PeerId n = 3 + seed % 3; // 3..5 nodes
        std::vector<PeerId> ids;
        for (PeerId i = 1; i <= n; ++i) {
            ids.push_back(i);
        }
        Cluster c(ids);
        std::map<PeerId, int> roundsLeft;
        std::map<PeerId, int> holdSteps;
        std::map<PeerId, bool> recorded;
        // Requests that are concurrently outstanding are granted in (ts, id)
        // order; check that per "batch" of simultaneous waiters.
        std::vector<std::pair<std::uint64_t, PeerId>> grants;
        for (const PeerId id : ids) {
            roundsLeft[id] = kRounds;
            c.request(id);
        }
        int steps = 0;
        std::size_t granted = 0;
        while (granted < ids.size() * kRounds && steps++ < 20000) {
            if (!c.inFlight.empty() && rng() % 4 != 0) {
                std::uniform_int_distribution<std::size_t> pick(0, c.inFlight.size() - 1);
                c.deliver(pick(rng)); // arbitrary reordering
            }
            ASSERT_LE(c.holders(), 1U) << "two holders at once, seed " << seed;
            for (const PeerId id : ids) {
                auto& node = c.at(id);
                if (!node.holding()) {
                    continue;
                }
                if (!recorded[id]) {
                    // Fairness: no other waiter may hold an older request.
                    for (const PeerId other : ids) {
                        const auto& o = c.at(other);
                        if (other != id && o.state() == RicartAgrawala::State::Wanted) {
                            EXPECT_LT(std::make_pair(node.requestTimestamp(), id),
                                      std::make_pair(o.requestTimestamp(), other))
                                << "an older request was overtaken, seed " << seed;
                        }
                    }
                    grants.emplace_back(node.requestTimestamp(), id);
                    recorded[id] = true;
                    holdSteps[id] = static_cast<int>(rng() % 6);
                    ++granted;
                }
                if (holdSteps[id]-- <= 0) {
                    c.release(id);
                    recorded[id] = false;
                    if (--roundsLeft[id] > 0) {
                        c.request(id);
                    }
                }
            }
        }
        ASSERT_EQ(granted, ids.size() * kRounds) << "starvation or deadlock, seed " << seed;
    }
}

// --- Hardening: replay and protocol violations ---

// Textbook RA counts any REPLY. Here a REPLY captured in round 1 and replayed
// in round 2 must not count, or a node could enter while another holds.
TEST(RicartAgrawalaTest, ReplayedReplyFromEarlierRoundIsRejected) {
    Cluster c({1, 2});
    c.request(1);
    c.deliver(0); // 2 gets REQUEST, replies
    const MutexMessage oldReply = c.inFlight.at(0);
    c.deliver(0);
    ASSERT_TRUE(c.at(1).holding());
    c.release(1);
    c.request(1); // round 2: REQUEST in flight, 2 has not answered
    auto replay = c.at(1).onMessage(oldReply);
    EXPECT_FALSE(replay.ok()) << "a stale reply was accepted";
    EXPECT_FALSE(c.at(1).holding());
}

TEST(RicartAgrawalaTest, ProtocolViolationsAreRejectedWithoutSideEffects) {
    auto ra = make(1, {2, 3});
    const auto clockBefore = ra.lamport();
    // unsolicited reply
    EXPECT_FALSE(
        ra.onMessage({.type = MutexMessage::Type::Reply, .from = 2, .to = 1, .timestamp = 5, .requestTs = 1}).ok());
    // non-member
    EXPECT_FALSE(
        ra.onMessage({.type = MutexMessage::Type::Request, .from = 9, .to = 1, .timestamp = 5, .requestTs = 5}).ok());
    // wrong recipient
    EXPECT_FALSE(
        ra.onMessage({.type = MutexMessage::Type::Request, .from = 2, .to = 3, .timestamp = 5, .requestTs = 5}).ok());
    // request whose ts fields disagree
    EXPECT_FALSE(
        ra.onMessage({.type = MutexMessage::Type::Request, .from = 2, .to = 1, .timestamp = 5, .requestTs = 6}).ok());
    // zero timestamp
    EXPECT_FALSE(
        ra.onMessage({.type = MutexMessage::Type::Request, .from = 2, .to = 1, .timestamp = 0, .requestTs = 0}).ok());
    EXPECT_EQ(ra.lamport(), clockBefore) << "a rejected message advanced the clock";
    EXPECT_EQ(ra.state(), RicartAgrawala::State::Released);
}

TEST(RicartAgrawalaTest, DuplicateReplyAndSecondPendingRequestAreRejected) {
    auto ra = make(1, {2, 3});
    auto req = ra.request();
    ASSERT_TRUE(req.ok());
    const auto ts = ra.requestTimestamp();
    const MutexMessage reply{.type = MutexMessage::Type::Reply, .from = 2, .to = 1, .timestamp = 9, .requestTs = ts};
    ASSERT_TRUE(ra.onMessage(reply).ok());
    EXPECT_FALSE(ra.onMessage(reply).ok()) << "the same peer counted twice";
    EXPECT_FALSE(ra.holding());

    // Peer 3 asks with a younger request: deferred. Asking again is a violation.
    const MutexMessage r3{.type = MutexMessage::Type::Request, .from = 3, .to = 1, .timestamp = 100, .requestTs = 100};
    ASSERT_TRUE(ra.onMessage(r3).ok());
    EXPECT_EQ(ra.deferredReplies(), 1U);
    EXPECT_FALSE(ra.onMessage(r3).ok());
}

TEST(RicartAgrawalaTest, MisuseOfRequestAndReleaseIsAnError) {
    auto ra = make(1, {2});
    EXPECT_FALSE(ra.release().ok());
    ASSERT_TRUE(ra.request().ok());
    EXPECT_FALSE(ra.request().ok());
    EXPECT_FALSE(ra.release().ok());
}

TEST(RicartAgrawalaTest, RemovingACrashedPeerUnblocksTheRequester) {
    Cluster c({1, 2, 3});
    c.request(1);
    // Deliver only to 2 (and its reply); 3 "crashed" and never answers.
    for (std::size_t i = 0; i < c.inFlight.size(); ++i) {
        if (c.inFlight[i].to == 2) {
            c.deliver(i);
            break;
        }
    }
    for (std::size_t i = 0; i < c.inFlight.size(); ++i) {
        if (c.inFlight[i].type == MutexMessage::Type::Reply) {
            c.deliver(i);
            break;
        }
    }
    EXPECT_FALSE(c.at(1).holding());
    c.at(1).removePeer(3); // failure detector: lease on 3 expired
    EXPECT_TRUE(c.at(1).holding());
}

TEST(RicartAgrawalaTest, SaturatedClockRefusesToOrderARequest) {
    auto ra = make(1, {2});
    ASSERT_TRUE(ra.onMessage({.type = MutexMessage::Type::Request,
                              .from = 2,
                              .to = 1,
                              .timestamp = psx::runtime::LamportClock::kMax,
                              .requestTs = psx::runtime::LamportClock::kMax})
                    .ok());
    EXPECT_FALSE(ra.request().ok());
}

TEST(MutexWireTest, RoundTripsAndRejectsMalformedBytes) {
    const MutexMessage m{.type = MutexMessage::Type::Reply,
                         .from = 7,
                         .to = 0xFFFFFFFFu,
                         .timestamp = 0x0102030405060708ULL,
                         .requestTs = 42};
    const std::string wire = encodeMutexMessage(m);
    ASSERT_EQ(wire.size(), 26U);
    auto back = decodeMutexMessage(wire);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), m);

    for (std::size_t n = 0; n < wire.size(); ++n) {
        EXPECT_FALSE(decodeMutexMessage(wire.substr(0, n)).ok()) << "truncated to " << n;
    }
    EXPECT_FALSE(decodeMutexMessage(wire + "x").ok());
    std::string badVersion = wire;
    badVersion[0] = 2;
    EXPECT_FALSE(decodeMutexMessage(badVersion).ok());
    std::string badType = wire;
    badType[1] = 9;
    EXPECT_FALSE(decodeMutexMessage(badType).ok());
}
