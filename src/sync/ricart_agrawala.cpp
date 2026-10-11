#include "psx/sync/ricart_agrawala.hpp"

#include "psx/transport/wire.hpp"

#include <tuple>

namespace psx::sync {

namespace {

constexpr std::uint8_t kMutexWireVersion = 1;
constexpr std::size_t kMutexWireSize = 1 + 1 + 4 + 4 + 8 + 8;

psx::Error violation(const char* what) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, what};
}

} // namespace

std::string encodeMutexMessage(const MutexMessage& message) {
    std::string out;
    out.reserve(kMutexWireSize);
    out.push_back(static_cast<char>(kMutexWireVersion));
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(message.type)));
    psx::transport::writeU32BE(out, message.from);
    psx::transport::writeU32BE(out, message.to);
    psx::transport::writeU64BE(out, message.timestamp);
    psx::transport::writeU64BE(out, message.requestTs);
    return out;
}

psx::Result<MutexMessage> decodeMutexMessage(std::string_view bytes) {
    // Exact size: no truncated reads, no trailing bytes smuggled along.
    if (bytes.size() != kMutexWireSize) {
        return violation("mutex message: wrong length");
    }
    if (static_cast<std::uint8_t>(bytes[0]) != kMutexWireVersion) {
        return violation("mutex message: unsupported version");
    }
    const auto type = static_cast<std::uint8_t>(bytes[1]);
    if (type != static_cast<std::uint8_t>(MutexMessage::Type::Request) &&
        type != static_cast<std::uint8_t>(MutexMessage::Type::Reply)) {
        return violation("mutex message: unknown type");
    }
    MutexMessage m;
    m.type = static_cast<MutexMessage::Type>(type);
    m.from = psx::transport::readU32BE(bytes.data() + 2);
    m.to = psx::transport::readU32BE(bytes.data() + 6);
    m.timestamp = psx::transport::readU64BE(bytes.data() + 10);
    m.requestTs = psx::transport::readU64BE(bytes.data() + 18);
    return m;
}

psx::Result<RicartAgrawala> RicartAgrawala::create(PeerId self, std::vector<PeerId> peers) {
    std::set<PeerId> unique;
    for (const PeerId peer : peers) {
        if (peer == self) {
            return violation("ricart_agrawala: peer list contains self");
        }
        if (!unique.insert(peer).second) {
            return violation("ricart_agrawala: duplicate peer");
        }
    }
    return RicartAgrawala(self, std::move(unique));
}

MutexMessage RicartAgrawala::reply(PeerId to, std::uint64_t answeredTs) {
    return MutexMessage{.type = MutexMessage::Type::Reply,
                        .from = self_,
                        .to = to,
                        .timestamp = clock_.tick(), // sending is an event
                        .requestTs = answeredTs};
}

psx::Result<std::vector<MutexMessage>> RicartAgrawala::request() {
    if (state_ != State::Released) {
        return violation("ricart_agrawala: request while already wanting or holding");
    }
    if (clock_.saturated()) {
        // A saturated clock can no longer produce a timestamp larger than the
        // last one, so it cannot order this request fairly. Refuse loudly.
        return violation("ricart_agrawala: lamport clock saturated");
    }
    requestTs_ = clock_.tick();
    pending_ = peers_;
    std::vector<MutexMessage> out;
    out.reserve(peers_.size());
    for (const PeerId peer : peers_) {
        out.push_back(MutexMessage{.type = MutexMessage::Type::Request,
                                   .from = self_,
                                   .to = peer,
                                   .timestamp = requestTs_,
                                   .requestTs = requestTs_});
    }
    state_ = pending_.empty() ? State::Held : State::Wanted;
    return out;
}

psx::Result<std::vector<MutexMessage>> RicartAgrawala::onMessage(const MutexMessage& message) {
    // Validate everything before touching any state, so a rejected message
    // leaves the node exactly as it was (including its Lamport clock).
    if (message.to != self_) {
        return violation("ricart_agrawala: message addressed to another node");
    }
    if (peers_.count(message.from) == 0) {
        return violation("ricart_agrawala: message from a non-member");
    }
    if (message.requestTs == 0) {
        return violation("ricart_agrawala: zero request timestamp");
    }

    if (message.type == MutexMessage::Type::Request) {
        if (message.timestamp != message.requestTs) {
            return violation("ricart_agrawala: request timestamp mismatch");
        }
        if (deferred_.count(Deferred{message.from, 0}) != 0) {
            // A participant has at most one outstanding request; a second one
            // while the first is still deferred is a replay or a bug.
            return violation("ricart_agrawala: second request while one is pending");
        }
        clock_.observe(message.timestamp);
        // Defer if we hold the section, or we want it and our request is
        // older: compare (ts, id) pairs, ties broken by the lower id.
        const bool weHavePriority =
            state_ == State::Held ||
            (state_ == State::Wanted && std::tie(requestTs_, self_) < std::tie(message.requestTs, message.from));
        if (weHavePriority) {
            deferred_.insert(Deferred{message.from, message.requestTs});
            return std::vector<MutexMessage>{};
        }
        return std::vector<MutexMessage>{reply(message.from, message.requestTs)};
    }

    // REPLY
    if (state_ != State::Wanted) {
        return violation("ricart_agrawala: unsolicited reply");
    }
    if (message.requestTs != requestTs_) {
        return violation("ricart_agrawala: reply answers a different request");
    }
    if (pending_.count(message.from) == 0) {
        return violation("ricart_agrawala: duplicate reply");
    }
    clock_.observe(message.timestamp);
    pending_.erase(message.from);
    if (pending_.empty()) {
        state_ = State::Held;
    }
    return std::vector<MutexMessage>{};
}

psx::Result<std::vector<MutexMessage>> RicartAgrawala::release() {
    if (state_ != State::Held) {
        return violation("ricart_agrawala: release without holding the section");
    }
    state_ = State::Released;
    requestTs_ = 0;
    std::vector<MutexMessage> out;
    out.reserve(deferred_.size());
    for (const Deferred& d : deferred_) {
        out.push_back(reply(d.peer, d.requestTs));
    }
    deferred_.clear();
    return out;
}

void RicartAgrawala::removePeer(PeerId peer) {
    peers_.erase(peer);
    deferred_.erase(Deferred{peer, 0});
    if (pending_.erase(peer) != 0 && state_ == State::Wanted && pending_.empty()) {
        state_ = State::Held;
    }
}

} // namespace psx::sync
