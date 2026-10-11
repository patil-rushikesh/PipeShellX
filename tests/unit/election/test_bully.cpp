#include "psx/election/bully.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace std::chrono_literals;
using psx::election::BullyNode;
using psx::election::Clock;
using psx::election::decodeElectionMessage;
using psx::election::ElectionMessage;
using psx::election::encodeElectionMessage;
using psx::election::PeerId;

namespace {

// Deterministic harness: instant delivery, virtual time advanced by hand,
// and nodes that can be "killed" (their traffic is dropped).
struct Group {
    std::map<PeerId, BullyNode> nodes;
    std::set<PeerId> dead;
    std::vector<ElectionMessage> inFlight;
    Clock::time_point now{};

    explicit Group(const std::vector<PeerId>& ids) {
        for (const PeerId id : ids) {
            std::vector<PeerId> peers;
            for (const PeerId o : ids) {
                if (o != id) {
                    peers.push_back(o);
                }
            }
            auto node = BullyNode::create(id, peers);
            EXPECT_TRUE(node.ok());
            nodes.emplace(id, std::move(node.value()));
        }
    }
    BullyNode& at(PeerId id) { return nodes.at(id); }
    void send(const std::vector<ElectionMessage>& out) { inFlight.insert(inFlight.end(), out.begin(), out.end()); }
    void flush() {
        while (!inFlight.empty()) {
            const ElectionMessage m = inFlight.front();
            inFlight.erase(inFlight.begin());
            if (dead.count(m.from) != 0 || dead.count(m.to) != 0) {
                continue; // a crashed node neither sends nor receives
            }
            auto out = at(m.to).onMessage(m, now);
            ASSERT_TRUE(out.ok()) << out.error().message();
            send(out.value());
        }
    }
    // Advance virtual time in small steps, polling every live node.
    void advance(std::chrono::milliseconds total) {
        for (auto t = 0ms; t < total; t += 10ms) {
            now += 10ms;
            for (auto& [id, node] : nodes) {
                if (dead.count(id) == 0) {
                    send(node.poll(now));
                }
            }
            flush();
        }
    }
    void boot() {
        for (auto& [id, node] : nodes) {
            send(node.startElection(now));
        }
        flush();
    }
    // Every live node agrees on `leader`, and the leader knows it leads.
    void expectAgreedLeader(PeerId leader) {
        for (auto& [id, node] : nodes) {
            if (dead.count(id) != 0) {
                continue;
            }
            ASSERT_TRUE(node.leader().has_value()) << "node " << id << " has no leader";
            EXPECT_EQ(*node.leader(), leader) << "node " << id;
        }
        EXPECT_EQ(at(leader).state(), BullyNode::State::Leader);
    }
};

} // namespace

TEST(BullyTest, CreateValidatesMembershipAndTimeouts) {
    EXPECT_FALSE(BullyNode::create(1, {1, 2}).ok());
    EXPECT_FALSE(BullyNode::create(1, {2, 2}).ok());
    EXPECT_FALSE(BullyNode::create(1, {2}, {.beaconInterval = 500ms, .leaderTimeout = 500ms}).ok())
        << "a leader timeout equal to the beacon period flaps forever";
    EXPECT_TRUE(BullyNode::create(1, {2, 3}).ok());
}

TEST(BullyTest, HighestIdBecomesLeaderImmediately) {
    auto node = BullyNode::create(3, {1, 2});
    ASSERT_TRUE(node.ok());
    const auto out = node.value().startElection(Clock::time_point{});
    EXPECT_EQ(node.value().state(), BullyNode::State::Leader);
    ASSERT_EQ(out.size(), 2U);
    for (const auto& m : out) {
        EXPECT_EQ(m.type, ElectionMessage::Type::Coordinator);
    }
}

TEST(BullyTest, AllNodesBootingTogetherAgreeOnTheHighestId) {
    Group g({1, 2, 3, 4, 5});
    g.boot();
    g.advance(2000ms);
    g.expectAgreedLeader(5);
}

TEST(BullyTest, LowestNodeElectionIsTakenOverByTheBully) {
    Group g({1, 2, 3});
    g.send(g.at(1).startElection(g.now)); // only the lowest node starts
    g.flush();
    g.advance(2000ms);
    g.expectAgreedLeader(3);
}

// Beacon protocol: followers detect the leader's silence and re-elect.
TEST(BullyTest, LeaderCrashIsDetectedByBeaconSilenceAndNextHighestTakesOver) {
    Group g({1, 2, 3, 4});
    g.boot();
    g.advance(1500ms);
    g.expectAgreedLeader(4);
    g.dead.insert(4); // kill -9 the leader
    g.advance(3000ms);
    g.expectAgreedLeader(3);
}

TEST(BullyTest, HigherNodeDyingMidElectionCausesARetry) {
    Group g({1, 2, 3});
    g.boot();
    g.advance(1500ms);
    g.expectAgreedLeader(3);
    // 3 answers 2's election, then dies before announcing.
    g.dead.insert(3);
    g.advance(4000ms);
    g.expectAgreedLeader(2);
}

TEST(BullyTest, RevivedHighestNodeBulliesItsWayBackToLeader) {
    Group g({1, 2, 3});
    g.boot();
    g.advance(1500ms);
    g.dead.insert(3);
    g.advance(3000ms);
    g.expectAgreedLeader(2);
    // 3 restarts with no memory (fresh state, epoch 0) and starts an election.
    auto fresh = BullyNode::create(3, {1, 2});
    ASSERT_TRUE(fresh.ok());
    g.nodes.erase(3);
    g.nodes.emplace(3, std::move(fresh.value()));
    g.dead.erase(3);
    g.send(g.at(3).startElection(g.now));
    g.flush();
    g.advance(3000ms);
    g.expectAgreedLeader(3);
}

// Epochs: a COORDINATOR delayed from an earlier epoch must not resurrect a
// leader that has since been replaced.
TEST(BullyTest, StaleCoordinatorFromAnOlderEpochIsIgnored) {
    Group g({1, 2, 3});
    g.boot();
    g.advance(1500ms);
    const ElectionMessage stale{.type = ElectionMessage::Type::Coordinator, .from = 3, .to = 1, .epoch = 0};
    g.dead.insert(3);
    g.advance(3000ms);
    g.expectAgreedLeader(2);
    auto out = g.at(1).onMessage(stale, g.now);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(*g.at(1).leader(), 2U) << "a stale announcement resurrected a dead leader";
}

TEST(BullyTest, LowerCoordinatorMakesALiveHigherNodeTakeOver) {
    auto node = BullyNode::create(3, {1, 2});
    ASSERT_TRUE(node.ok());
    auto out = node.value().onMessage({.type = ElectionMessage::Type::Coordinator, .from = 1, .to = 3, .epoch = 5},
                                      Clock::time_point{});
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(node.value().state(), BullyNode::State::Leader);
    EXPECT_GT(node.value().epoch(), 5U);
}

TEST(BullyTest, ImpossibleMessagesAreRejected) {
    auto node = BullyNode::create(2, {1, 3});
    ASSERT_TRUE(node.ok());
    auto& n = node.value();
    const Clock::time_point t{};
    EXPECT_FALSE(n.onMessage({.type = ElectionMessage::Type::Election, .from = 3, .to = 2}, t).ok())
        << "ELECTION only ever goes to higher ids";
    EXPECT_FALSE(n.onMessage({.type = ElectionMessage::Type::Answer, .from = 1, .to = 2}, t).ok())
        << "ANSWER only ever comes from higher ids";
    EXPECT_FALSE(n.onMessage({.type = ElectionMessage::Type::Coordinator, .from = 9, .to = 2}, t).ok());
    EXPECT_FALSE(n.onMessage({.type = ElectionMessage::Type::Coordinator, .from = 3, .to = 1}, t).ok());
    EXPECT_EQ(n.state(), BullyNode::State::Follower);
    EXPECT_FALSE(n.leader().has_value());
}

TEST(BullyTest, EpochSaturatesInsteadOfWrapping) {
    auto node = BullyNode::create(2, {1});
    ASSERT_TRUE(node.ok());
    const auto max = std::numeric_limits<std::uint64_t>::max();
    ASSERT_TRUE(
        node.value()
            .onMessage({.type = ElectionMessage::Type::Election, .from = 1, .to = 2, .epoch = max}, Clock::time_point{})
            .ok());
    EXPECT_EQ(node.value().epoch(), max);
}

TEST(ElectionWireTest, RoundTripsAndRejectsMalformedBytes) {
    const ElectionMessage m{.type = ElectionMessage::Type::Beacon, .from = 4, .to = 9, .epoch = 77};
    const std::string wire = encodeElectionMessage(m);
    ASSERT_EQ(wire.size(), 18U);
    auto back = decodeElectionMessage(wire);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value(), m);
    for (std::size_t n = 0; n < wire.size(); ++n) {
        EXPECT_FALSE(decodeElectionMessage(wire.substr(0, n)).ok());
    }
    EXPECT_FALSE(decodeElectionMessage(wire + "!").ok());
    for (const char badType : {char(0), char(5), char(0x7f)}) {
        std::string w = wire;
        w[1] = badType;
        EXPECT_FALSE(decodeElectionMessage(w).ok());
    }
    std::string badVersion = wire;
    badVersion[0] = 9;
    EXPECT_FALSE(decodeElectionMessage(badVersion).ok());
}
