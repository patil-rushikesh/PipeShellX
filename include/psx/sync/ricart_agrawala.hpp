#pragma once

// Ricart-Agrawala distributed mutual exclusion (1981), as a transport-agnostic
// ("sans-IO") state machine, the same shape as psx::transport::Session: feed it
// events, it returns the messages to send. No sockets, threads or timers live
// in here, so every interleaving is reproducible in a unit test or in the
// simulator (psx::sim), and a real transport can drive it unchanged.
//
// Problem it solves in PipeShellX: several controller instances (two
// operators, two schedulers) can target the same fleet. A fleet-wide change
// such as a rolling restart is a critical section: two of them overlapping
// can take the whole fleet down at once. RA lets the controllers agree on who
// goes first, with no central lock server to become a single point of failure.
//
// Algorithm (N participants, 2(N-1) messages per entry):
//   request(): tick the Lamport clock, remember (ts, self), send REQUEST(ts)
//              to every peer, wait for a REPLY from each.
//   on REQUEST(ts', j): reply immediately unless we hold the section, or we
//              want it and our (ts, self) < (ts', j) -- then defer the reply.
//   release(): send every deferred REPLY.
// The (Lamport ts, id) pair is a total order, so requests are served in
// timestamp order: no starvation, and no deadlock (no cycle of waiting).
//
// Hardening beyond the textbook (see docs/ds-project/07-security-review.md):
//   * A REPLY echoes the timestamp of the request it answers; a replayed or
//     stale REPLY from an earlier round is rejected instead of counting
//     toward the current one (textbook RA would admit two holders).
//   * Messages from unknown peers, to the wrong recipient, duplicate REPLYs
//     and unsolicited REPLYs are protocol errors, not silently accepted.
//   * Sender identity (`from`) MUST be authenticated by the transport (e.g.
//     the mTLS SAN of the connection); this class trusts what it is given.
//
// Failure model: RA needs a REPLY from every peer, so one crashed peer blocks
// everyone. removePeer() takes a peer out of the group when a failure
// detector (the lease/heartbeat) declares it dead. That is only safe if the
// detector is accurate: a partitioned-but-alive peer could still be inside
// the section. This is the standard limitation of RA, stated, not hidden.
//
// Not thread-safe (single reactor thread), like every runtime object here.

#include "psx/result.hpp"
#include "psx/runtime/lamport_clock.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace psx::sync {

using PeerId = std::uint32_t;

struct MutexMessage {
    enum class Type : std::uint8_t { Request = 1, Reply = 2 };
    Type type = Type::Request;
    PeerId from = 0;
    PeerId to = 0;
    std::uint64_t timestamp = 0; // sender's Lamport clock when it sent this
    std::uint64_t requestTs = 0; // REQUEST: the request's ts; REPLY: the ts being answered

    bool operator==(const MutexMessage&) const = default;
};

// Fixed 26-byte wire encoding, big-endian, versioned like OPEN:
//   u8 version(=1) | u8 type | u32 from | u32 to | u64 timestamp | u64 requestTs
std::string encodeMutexMessage(const MutexMessage& message);
psx::Result<MutexMessage> decodeMutexMessage(std::string_view bytes);

class RicartAgrawala {
public:
    enum class State : std::uint8_t { Released, Wanted, Held };

    // `peers` are the other participants; must not contain `self` or repeats.
    static psx::Result<RicartAgrawala> create(PeerId self, std::vector<PeerId> peers);

    // Ask for the critical section. Returns the REQUESTs to send (empty if
    // there are no peers, in which case the section is granted at once).
    // Error if a request is already outstanding or the section is held.
    psx::Result<std::vector<MutexMessage>> request();

    // Handle one inbound message; returns the messages to send in response.
    // An error means the message violated the protocol and was not applied.
    psx::Result<std::vector<MutexMessage>> onMessage(const MutexMessage& message);

    // Leave the critical section; returns the REPLYs that were deferred.
    psx::Result<std::vector<MutexMessage>> release();

    // A failure detector declared `peer` dead: stop waiting for it and drop
    // its deferred request. May complete our own request (check holding()).
    void removePeer(PeerId peer);

    State state() const noexcept { return state_; }
    bool holding() const noexcept { return state_ == State::Held; }
    PeerId self() const noexcept { return self_; }
    std::uint64_t requestTimestamp() const noexcept { return requestTs_; }
    std::size_t awaitingReplies() const noexcept { return pending_.size(); }
    std::size_t deferredReplies() const noexcept { return deferred_.size(); }
    std::uint64_t lamport() const noexcept { return clock_.value(); }

private:
    RicartAgrawala(PeerId self, std::set<PeerId> peers) : self_(self), peers_(std::move(peers)) {}

    struct Deferred {
        PeerId peer;
        std::uint64_t requestTs;
        bool operator<(const Deferred& o) const { return peer < o.peer; }
    };

    MutexMessage reply(PeerId to, std::uint64_t answeredTs);

    PeerId self_;
    std::set<PeerId> peers_;
    psx::runtime::LamportClock clock_;
    State state_ = State::Released;
    std::uint64_t requestTs_ = 0;
    std::set<PeerId> pending_;    // peers we still need a REPLY from
    std::set<Deferred> deferred_; // requests we will answer on release()
};

} // namespace psx::sync
