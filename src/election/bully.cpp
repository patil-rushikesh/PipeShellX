#include "psx/election/bully.hpp"

#include "psx/transport/wire.hpp"

#include <algorithm>
#include <limits>

namespace psx::election {

namespace {

constexpr std::uint8_t kElectionWireVersion = 1;
constexpr std::size_t kElectionWireSize = 1 + 1 + 4 + 4 + 8;

psx::Error violation(const char* what) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, what};
}

std::uint64_t nextEpoch(std::uint64_t epoch) {
    // Saturate rather than wrap: a wrapped epoch of 0 would make every
    // legitimate message look stale and stall elections forever.
    return epoch == std::numeric_limits<std::uint64_t>::max() ? epoch : epoch + 1;
}

} // namespace

std::string encodeElectionMessage(const ElectionMessage& message) {
    std::string out;
    out.reserve(kElectionWireSize);
    out.push_back(static_cast<char>(kElectionWireVersion));
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(message.type)));
    psx::transport::writeU32BE(out, message.from);
    psx::transport::writeU32BE(out, message.to);
    psx::transport::writeU64BE(out, message.epoch);
    return out;
}

psx::Result<ElectionMessage> decodeElectionMessage(std::string_view bytes) {
    if (bytes.size() != kElectionWireSize) {
        return violation("election message: wrong length");
    }
    if (static_cast<std::uint8_t>(bytes[0]) != kElectionWireVersion) {
        return violation("election message: unsupported version");
    }
    const auto type = static_cast<std::uint8_t>(bytes[1]);
    if (type < static_cast<std::uint8_t>(ElectionMessage::Type::Election) ||
        type > static_cast<std::uint8_t>(ElectionMessage::Type::Beacon)) {
        return violation("election message: unknown type");
    }
    ElectionMessage m;
    m.type = static_cast<ElectionMessage::Type>(type);
    m.from = psx::transport::readU32BE(bytes.data() + 2);
    m.to = psx::transport::readU32BE(bytes.data() + 6);
    m.epoch = psx::transport::readU64BE(bytes.data() + 10);
    return m;
}

psx::Result<BullyNode> BullyNode::create(PeerId self, std::vector<PeerId> peers, Timeouts timeouts) {
    std::set<PeerId> unique;
    for (const PeerId peer : peers) {
        if (peer == self) {
            return violation("bully: peer list contains self");
        }
        if (!unique.insert(peer).second) {
            return violation("bully: duplicate peer");
        }
    }
    if (timeouts.answerTimeout.count() <= 0 || timeouts.coordinatorTimeout.count() <= 0 ||
        timeouts.beaconInterval.count() <= 0 || timeouts.leaderTimeout <= timeouts.beaconInterval) {
        // A leader timeout no longer than the beacon period would declare a
        // healthy leader dead between two beacons: permanent re-elections.
        return violation("bully: invalid timeouts (need leaderTimeout > beaconInterval > 0)");
    }
    return BullyNode(self, std::move(unique), timeouts);
}

ElectionMessage BullyNode::to(PeerId peer, ElectionMessage::Type type) const {
    return ElectionMessage{.type = type, .from = self_, .to = peer, .epoch = epoch_};
}

std::vector<ElectionMessage> BullyNode::startElection(Clock::time_point now) {
    epoch_ = nextEpoch(epoch_);
    electionEpoch_ = epoch_;
    leader_.reset();
    std::vector<ElectionMessage> out;
    for (const PeerId peer : peers_) {
        if (peer > self_) {
            out.push_back(to(peer, ElectionMessage::Type::Election));
        }
    }
    if (out.empty()) {
        return becomeLeader(now); // nobody outranks us
    }
    state_ = State::Electing;
    deadline_ = now + timeouts_.answerTimeout;
    return out;
}

std::vector<ElectionMessage> BullyNode::becomeLeader(Clock::time_point now) {
    state_ = State::Leader;
    leader_ = self_;
    nextBeacon_ = now + timeouts_.beaconInterval;
    std::vector<ElectionMessage> out;
    out.reserve(peers_.size());
    for (const PeerId peer : peers_) {
        // Announce to everyone, higher ids included: a higher node that is in
        // fact alive (just slow) will see a lower COORDINATOR and take over.
        out.push_back(to(peer, ElectionMessage::Type::Coordinator));
    }
    return out;
}

psx::Result<std::vector<ElectionMessage>> BullyNode::onMessage(const ElectionMessage& message, Clock::time_point now) {
    if (message.to != self_) {
        return violation("bully: message addressed to another node");
    }
    if (peers_.count(message.from) == 0) {
        return violation("bully: message from a non-member");
    }

    switch (message.type) {
        case ElectionMessage::Type::Election: {
            if (message.from > self_) {
                return violation("bully: ELECTION from a higher id");
            }
            epoch_ = std::max(epoch_, message.epoch);
            // ANSWER echoes the election's epoch so the sender can match it.
            std::vector<ElectionMessage> out{ElectionMessage{
                .type = ElectionMessage::Type::Answer, .from = self_, .to = message.from, .epoch = message.epoch}};
            if (state_ == State::Leader) {
                // We are alive and already lead: tell the sender directly.
                out.push_back(to(message.from, ElectionMessage::Type::Coordinator));
            } else if (state_ == State::Follower) {
                auto own = startElection(now); // the bully step: take over
                out.insert(out.end(), own.begin(), own.end());
            }
            return out;
        }
        case ElectionMessage::Type::Answer: {
            if (message.from < self_) {
                return violation("bully: ANSWER from a lower id");
            }
            if (state_ == State::Electing && message.epoch == electionEpoch_) {
                state_ = State::AwaitingCoordinator;
                deadline_ = now + timeouts_.coordinatorTimeout;
            }
            return std::vector<ElectionMessage>{}; // stale or duplicate: ignore
        }
        case ElectionMessage::Type::Coordinator: {
            if (message.epoch < epoch_) {
                return std::vector<ElectionMessage>{}; // from an older epoch: stale
            }
            epoch_ = message.epoch;
            if (message.from < self_) {
                return startElection(now); // a lower node claims to lead while we live
            }
            state_ = State::Follower;
            leader_ = message.from;
            lastLeaderContact_ = now;
            return std::vector<ElectionMessage>{};
        }
        case ElectionMessage::Type::Beacon: {
            if (message.epoch < epoch_) {
                return std::vector<ElectionMessage>{}; // a deposed leader's beacon
            }
            if (leader_ && *leader_ == message.from) {
                lastLeaderContact_ = now;
                return std::vector<ElectionMessage>{};
            }
            epoch_ = message.epoch;
            if (message.from < self_) {
                return startElection(now);
            }
            // A higher leader we never heard announce itself (we joined late).
            state_ = State::Follower;
            leader_ = message.from;
            lastLeaderContact_ = now;
            return std::vector<ElectionMessage>{};
        }
    }
    return violation("bully: unknown message type");
}

std::vector<ElectionMessage> BullyNode::poll(Clock::time_point now) {
    switch (state_) {
        case State::Electing:
            if (now >= deadline_) {
                return becomeLeader(now); // no higher node answered
            }
            break;
        case State::AwaitingCoordinator:
            if (now >= deadline_) {
                return startElection(now); // the higher node died mid-election
            }
            break;
        case State::Leader:
            if (now >= nextBeacon_) {
                nextBeacon_ = now + timeouts_.beaconInterval; // no burst catch-up
                std::vector<ElectionMessage> out;
                for (const PeerId peer : peers_) {
                    out.push_back(to(peer, ElectionMessage::Type::Beacon));
                }
                return out;
            }
            break;
        case State::Follower:
            if (leader_ && now - lastLeaderContact_ >= timeouts_.leaderTimeout) {
                return startElection(now); // beacon silence: leader presumed dead
            }
            break;
    }
    return {};
}

std::optional<Clock::time_point> BullyNode::nextDeadline() const noexcept {
    switch (state_) {
        case State::Electing:
        case State::AwaitingCoordinator:
            return deadline_;
        case State::Leader:
            return nextBeacon_;
        case State::Follower:
            if (leader_) {
                return lastLeaderContact_ + timeouts_.leaderTimeout;
            }
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace psx::election
