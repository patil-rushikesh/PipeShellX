#pragma once

// Bully leader election (Garcia-Molina 1982) with a leader beacon, as a
// transport-agnostic ("sans-IO") state machine. Time is passed in explicitly
// (`now`), so the simulator and unit tests drive it with a virtual clock and
// every failure scenario is reproducible.
//
// Problem it solves in PipeShellX: today there is exactly one controller; if
// it dies, scheduled/recurring fleet work stops. With several controller
// instances, exactly one should act as the coordinator (the scheduler that
// dispatches recurring jobs) and another must take over when it fails.
//
// Messages (one small fixed-size wire format, see encodeElectionMessage):
//   ELECTION     -> sent to every higher id: "I am starting an election"
//   ANSWER       -> higher id to lower id: "I am alive, stand down"
//   COORDINATOR  -> the winner to everyone: "I am the leader"
//   BEACON       -> the leader to everyone, periodically: "still alive"
//
// Rules:
//   * startElection(): send ELECTION to every higher id. No ANSWER before
//     answerTimeout -> become leader and announce COORDINATOR to everyone.
//     An ANSWER arrives -> wait coordinatorTimeout for COORDINATOR, else retry.
//   * on ELECTION from a lower id: ANSWER it, then start our own election
//     (that is the "bully" step: the higher node takes over).
//   * on COORDINATOR from a lower id while we are alive: we outrank it, so we
//     start an election and take over.
//   * Beacon protocol: the leader sends BEACON every beaconInterval; a
//     follower that hears nothing from the leader for leaderTimeout declares
//     it failed and starts an election (failure detection).
//
// Hardening beyond the textbook (see docs/ds-project/07-security-review.md):
//   * Every message carries an election epoch (like Raft's term). A delayed
//     COORDINATOR or BEACON from an earlier epoch -- e.g. from a leader that
//     has since crashed -- is ignored instead of resurrecting it.
//   * ELECTION from a higher id or ANSWER from a lower id cannot happen in a
//     correct run; they are rejected as protocol violations.
//   * Sender identity (`from`) MUST be authenticated by the transport (mTLS
//     SAN); a forged `from` with a huge id could otherwise win every election.
//
// Not thread-safe (single reactor thread).

#include "psx/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace psx::election {

using PeerId = std::uint32_t;
using Clock = std::chrono::steady_clock;

struct ElectionMessage {
    enum class Type : std::uint8_t { Election = 1, Answer = 2, Coordinator = 3, Beacon = 4 };
    Type type = Type::Election;
    PeerId from = 0;
    PeerId to = 0;
    std::uint64_t epoch = 0;

    bool operator==(const ElectionMessage&) const = default;
};

// Fixed 18-byte wire encoding, big-endian:
//   u8 version(=1) | u8 type | u32 from | u32 to | u64 epoch
std::string encodeElectionMessage(const ElectionMessage& message);
psx::Result<ElectionMessage> decodeElectionMessage(std::string_view bytes);

struct Timeouts {
    std::chrono::milliseconds answerTimeout{300};      // wait for ANSWER after ELECTION
    std::chrono::milliseconds coordinatorTimeout{900}; // wait for COORDINATOR after ANSWER
    std::chrono::milliseconds beaconInterval{200};     // leader beacon period
    std::chrono::milliseconds leaderTimeout{700};      // silence before declaring the leader dead
};

class BullyNode {
public:
    enum class State : std::uint8_t { Follower, Electing, AwaitingCoordinator, Leader };

    static psx::Result<BullyNode> create(PeerId self, std::vector<PeerId> peers, Timeouts timeouts = {});

    // Begin an election (also called internally on leader failure).
    std::vector<ElectionMessage> startElection(Clock::time_point now);

    // Handle one inbound message; returns the messages to send.
    psx::Result<std::vector<ElectionMessage>> onMessage(const ElectionMessage& message, Clock::time_point now);

    // Advance timers: answer/coordinator timeouts, leader beacons, and
    // leader-failure detection. Call whenever time passes.
    std::vector<ElectionMessage> poll(Clock::time_point now);

    State state() const noexcept { return state_; }
    std::optional<PeerId> leader() const noexcept { return leader_; }
    PeerId self() const noexcept { return self_; }
    std::uint64_t epoch() const noexcept { return epoch_; }
    // Earliest time poll() has work to do (for a reactor timer); nullopt = none.
    std::optional<Clock::time_point> nextDeadline() const noexcept;

private:
    BullyNode(PeerId self, std::set<PeerId> peers, Timeouts timeouts)
        : self_(self), peers_(std::move(peers)), timeouts_(timeouts) {}

    std::vector<ElectionMessage> becomeLeader(Clock::time_point now);
    ElectionMessage to(PeerId peer, ElectionMessage::Type type) const;

    PeerId self_;
    std::set<PeerId> peers_;
    Timeouts timeouts_;
    State state_ = State::Follower;
    std::optional<PeerId> leader_;
    std::uint64_t epoch_ = 0;         // highest epoch seen or used
    std::uint64_t electionEpoch_ = 0; // epoch of the election we are running
    Clock::time_point deadline_{};    // Electing / AwaitingCoordinator timeout
    Clock::time_point nextBeacon_{};  // Leader: when to beacon next
    Clock::time_point lastLeaderContact_{};
};

} // namespace psx::election
