#include "psx/sim/simulation.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>

namespace psx::sim {

namespace {

using std::chrono::milliseconds;

// A scheduled action in virtual time. `seq` keeps same-time actions in the
// order they were scheduled, so a run is fully determined by its seed.
struct Action {
    enum class Kind : std::uint8_t { Deliver, Request, Release, Kill, Detect, Revive };
    std::int64_t at = 0;
    std::uint64_t seq = 0;
    Kind kind = Kind::Deliver;
    std::uint32_t node = 0; // recipient / actor
    std::string bytes;      // Deliver: the encoded message
    bool operator>(const Action& o) const { return at > o.at || (at == o.at && seq > o.seq); }
};

class Scheduler {
public:
    explicit Scheduler(const NetworkConfig& net) : rng_(net.seed), net_(net) {}

    void at(std::int64_t time, Action::Kind kind, std::uint32_t node, std::string bytes = {}) {
        queue_.push(Action{time, seq_++, kind, node, std::move(bytes)});
    }
    // Network delay is drawn per message, so two messages sent in order can
    // arrive in the opposite order -- the reordering the algorithms must survive.
    std::int64_t networkDelay() {
        std::uniform_int_distribution<std::int64_t> d(net_.minDelay.count(),
                                                      std::max(net_.minDelay.count(), net_.maxDelay.count()));
        return d(rng_);
    }
    std::int64_t randomBelow(std::int64_t bound) {
        if (bound <= 0) {
            return 0;
        }
        std::uniform_int_distribution<std::int64_t> d(0, bound);
        return d(rng_);
    }
    bool empty() const { return queue_.empty(); }
    std::int64_t nextTime() const { return queue_.top().at; }
    Action pop() {
        Action a = queue_.top();
        queue_.pop();
        return a;
    }

private:
    std::priority_queue<Action, std::vector<Action>, std::greater<Action>> queue_;
    std::uint64_t seq_ = 0;
    std::mt19937 rng_;
    NetworkConfig net_;
};

const char* name(psx::election::ElectionMessage::Type t) {
    using T = psx::election::ElectionMessage::Type;
    switch (t) {
        case T::Election:
            return "ELECTION";
        case T::Answer:
            return "ANSWER";
        case T::Coordinator:
            return "COORDINATOR";
        case T::Beacon:
            return "BEACON";
    }
    return "?";
}

const char* name(psx::sync::MutexMessage::Type t) {
    return t == psx::sync::MutexMessage::Type::Request ? "REQUEST" : "REPLY";
}

} // namespace

// ======================================================================= election

ElectionReport runElection(const ElectionConfig& config) {
    using psx::election::BullyNode;
    using psx::election::ElectionMessage;
    using psx::election::PeerId;

    ElectionReport report;
    const std::uint32_t n = std::max<std::uint32_t>(config.nodes, 1);
    const auto base = psx::election::Clock::time_point{};
    auto clockAt = [&](std::int64_t t) { return base + milliseconds(t); };

    auto makeNode = [&](PeerId id) {
        std::vector<PeerId> peers;
        for (PeerId p = 1; p <= n; ++p) {
            if (p != id) {
                peers.push_back(p);
            }
        }
        return BullyNode::create(id, peers, config.timeouts);
    };

    std::map<PeerId, BullyNode> nodes;
    std::set<PeerId> dead;
    for (PeerId id = 1; id <= n; ++id) {
        auto node = makeNode(id);
        if (!node.ok()) {
            report.events.push_back({0, id, 0, std::string("invalid configuration: ") + node.error().message()});
            return report;
        }
        nodes.emplace(id, std::move(node.value()));
    }

    Scheduler sched(config.network);
    std::int64_t now = 0;
    std::map<PeerId, std::optional<PeerId>> lastLeader;
    std::map<PeerId, BullyNode::State> lastState;
    auto log = [&](PeerId node, std::uint64_t epoch, std::string text) {
        report.events.push_back({now, node, epoch, std::move(text)});
    };

    auto send = [&](const std::vector<ElectionMessage>& out) {
        for (const auto& m : out) {
            if (dead.count(m.from) != 0) {
                continue;
            }
            ++report.messagesSent;
            if (config.traceMessages && m.type != ElectionMessage::Type::Beacon) {
                std::ostringstream t;
                t << "send " << name(m.type) << " -> node " << m.to;
                log(m.from, m.epoch, t.str());
            }
            sched.at(now + sched.networkDelay(), Action::Kind::Deliver, m.to,
                     psx::election::encodeElectionMessage(m)); // real wire bytes
        }
    };

    // Log state/leader changes once, after each action.
    auto observe = [&] {
        for (auto& [id, node] : nodes) {
            if (dead.count(id) != 0) {
                continue;
            }
            if (lastState[id] != node.state() && node.state() == BullyNode::State::Electing) {
                ++report.electionsStarted;
                log(id, node.epoch(), "starts an ELECTION");
            }
            if (lastLeader[id] != node.leader()) {
                if (node.leader() && *node.leader() == id) {
                    log(id, node.epoch(), "*** becomes LEADER ***");
                } else if (node.leader()) {
                    log(id, node.epoch(), "follows leader node " + std::to_string(*node.leader()));
                }
            }
            lastState[id] = node.state();
            lastLeader[id] = node.leader();
        }
    };

    auto highestAlive = [&] {
        PeerId best = 0;
        for (PeerId id = 1; id <= n; ++id) {
            if (dead.count(id) == 0) {
                best = id;
            }
        }
        return best;
    };
    auto allAgree = [&] {
        const PeerId expect = highestAlive();
        if (expect == 0 || nodes.at(expect).state() != BullyNode::State::Leader) {
            return false;
        }
        for (auto& [id, node] : nodes) {
            if (dead.count(id) == 0 && node.leader() != std::optional<PeerId>(expect)) {
                return false;
            }
        }
        return true;
    };

    // Boot: every node starts an election at t=0 -> concurrent elections.
    for (auto& [id, node] : nodes) {
        send(node.startElection(clockAt(now)));
    }
    observe();
    if (config.killLeaderAt) {
        sched.at(config.killLeaderAt->count(), Action::Kind::Kill, 0);
    }
    if (config.reviveAt) {
        sched.at(config.reviveAt->count(), Action::Kind::Revive, 0);
    }

    const std::int64_t end = config.duration.count();
    while (now <= end) {
        // Next moment anything happens: a message, a scheduled action, or a timer.
        std::int64_t next = end + 1;
        if (!sched.empty()) {
            next = std::min(next, sched.nextTime());
        }
        for (auto& [id, node] : nodes) {
            if (dead.count(id) == 0) {
                if (const auto d = node.nextDeadline()) {
                    next = std::min(next, std::chrono::duration_cast<milliseconds>(*d - base).count());
                }
            }
        }
        if (next > end) {
            break;
        }
        now = std::max(now, next);

        while (!sched.empty() && sched.nextTime() <= now) {
            Action a = sched.pop();
            switch (a.kind) {
                case Action::Kind::Deliver: {
                    auto decoded = psx::election::decodeElectionMessage(a.bytes);
                    if (!decoded.ok()) {
                        log(a.node, 0, "dropped undecodable message");
                        break;
                    }
                    const ElectionMessage& m = decoded.value();
                    if (dead.count(m.to) != 0 || dead.count(m.from) != 0) {
                        break; // lost: a crashed node neither sends nor receives
                    }
                    auto out = nodes.at(m.to).onMessage(m, clockAt(now));
                    if (!out.ok()) {
                        log(m.to, 0, std::string("rejected ") + name(m.type) + ": " + out.error().message());
                        break;
                    }
                    send(out.value());
                    break;
                }
                case Action::Kind::Kill: {
                    PeerId victim = 0;
                    for (auto& [id, node] : nodes) {
                        if (dead.count(id) == 0 && node.state() == BullyNode::State::Leader) {
                            victim = std::max(victim, id);
                        }
                    }
                    if (victim == 0) {
                        victim = highestAlive();
                    }
                    if (victim != 0) {
                        dead.insert(victim);
                        report.killedNode = victim;
                        log(victim, 0, "!!! CRASHED (kill -9) !!!");
                    }
                    break;
                }
                case Action::Kind::Revive: {
                    if (report.killedNode && dead.count(*report.killedNode) != 0) {
                        const PeerId id = *report.killedNode;
                        auto fresh = makeNode(id); // restarts with no memory
                        nodes.erase(id);
                        nodes.emplace(id, std::move(fresh.value()));
                        dead.erase(id);
                        lastLeader[id].reset();
                        lastState[id] = BullyNode::State::Follower;
                        log(id, 0, "restarted (fresh state)");
                        send(nodes.at(id).startElection(clockAt(now)));
                    }
                    break;
                }
                default:
                    break;
            }
        }
        for (auto& [id, node] : nodes) {
            if (dead.count(id) == 0) {
                send(node.poll(clockAt(now)));
            }
        }
        observe();
        if (report.killedNode && !report.recoveredAtMs && dead.count(*report.killedNode) != 0 && allAgree()) {
            report.recoveredAtMs = now;
            log(highestAlive(), nodes.at(highestAlive()).epoch(), "cluster agrees on the new leader");
        }
    }

    report.expectedLeader = highestAlive();
    report.finalLeaders.assign(n, std::nullopt);
    for (auto& [id, node] : nodes) {
        if (dead.count(id) == 0) {
            report.finalLeaders[id - 1] = node.leader();
        }
    }
    report.agreed = allAgree();
    return report;
}

// ========================================================================== mutex

MutexReport runMutex(const MutexConfig& config) {
    using psx::sync::MutexMessage;
    using psx::sync::PeerId;
    using psx::sync::RicartAgrawala;

    MutexReport report;
    const std::uint32_t n = std::max<std::uint32_t>(config.nodes, 1);
    std::map<PeerId, RicartAgrawala> nodes;
    for (PeerId id = 1; id <= n; ++id) {
        std::vector<PeerId> peers;
        for (PeerId p = 1; p <= n; ++p) {
            if (p != id) {
                peers.push_back(p);
            }
        }
        nodes.emplace(id, std::move(RicartAgrawala::create(id, peers).value()));
    }

    Scheduler sched(config.network);
    std::int64_t now = 0;
    std::set<PeerId> dead;
    std::map<PeerId, std::uint32_t> issued;   // requests made
    std::map<PeerId, bool> inside;            // currently in the critical section
    std::map<PeerId, std::size_t> entryIndex; // index into report.entries
    auto log = [&](PeerId node, std::string text) {
        const std::uint64_t l = config.naive ? 0 : nodes.at(node).lamport();
        report.events.push_back({now, node, l, std::move(text)});
    };
    auto send = [&](const std::vector<MutexMessage>& out) {
        for (const auto& m : out) {
            if (dead.count(m.from) != 0) {
                continue;
            }
            ++report.messagesSent;
            if (config.traceMessages) {
                std::ostringstream t;
                t << "send " << name(m.type) << "(req ts=" << m.requestTs << ") -> node " << m.to;
                log(m.from, t.str());
            }
            sched.at(now + sched.networkDelay(), Action::Kind::Deliver, m.to, psx::sync::encodeMutexMessage(m));
        }
    };
    auto holders = [&] {
        std::uint32_t c = 0;
        for (const auto& [id, in] : inside) {
            if (in && dead.count(id) == 0) {
                ++c;
            }
        }
        return c;
    };
    // After every action: detect new entries, enforce the safety invariant.
    std::uint32_t lastHolders = 0;
    auto check = [&] {
        for (auto& [id, node] : nodes) {
            if (dead.count(id) != 0 || config.naive || inside[id] || !node.holding()) {
                continue;
            }
            // Fairness: nobody still waiting may hold an older (ts, id).
            for (auto& [other, o] : nodes) {
                if (other != id && dead.count(other) == 0 && o.state() == RicartAgrawala::State::Wanted &&
                    std::make_pair(o.requestTimestamp(), other) < std::make_pair(node.requestTimestamp(), id)) {
                    report.grantedInTimestampOrder = false;
                }
            }
            inside[id] = true;
            entryIndex[id] = report.entries.size();
            report.entries.push_back({id, node.requestTimestamp(), now, 0});
            log(id, "==> ENTERS critical section (request ts=" + std::to_string(node.requestTimestamp()) + ")");
            sched.at(now + config.csDuration.count(), Action::Kind::Release, id);
        }
        const std::uint32_t h = holders();
        report.maxConcurrentHolders = std::max(report.maxConcurrentHolders, h);
        if (h > 1 && h > lastHolders) {
            ++report.violations;
            std::string who;
            for (const auto& [id, in] : inside) {
                if (in && dead.count(id) == 0) {
                    who += (who.empty() ? "" : ",") + std::to_string(id);
                }
            }
            report.events.push_back({now, 0, 0, "!!! SAFETY VIOLATION: nodes " + who + " are inside together !!!"});
        }
        lastHolders = h;
    };

    // Every node wants the section soon after start -> real contention.
    for (PeerId id = 1; id <= n; ++id) {
        if (config.requestsPerNode > 0) {
            sched.at(sched.randomBelow(config.maxThink.count() / 3), Action::Kind::Request, id);
        }
    }
    if (config.killNode && *config.killNode >= 1 && *config.killNode <= n) {
        sched.at(config.killAt.count(), Action::Kind::Kill, *config.killNode);
        sched.at(config.killAt.count() + config.detectAfter.count(), Action::Kind::Detect, *config.killNode);
    }

    constexpr std::int64_t kHardStopMs = 10 * 60 * 1000; // a stuck run cannot loop forever
    while (!sched.empty() && sched.nextTime() <= kHardStopMs) {
        Action a = sched.pop();
        now = a.at;
        switch (a.kind) {
            case Action::Kind::Request: {
                if (dead.count(a.node) != 0) {
                    break;
                }
                ++issued[a.node];
                if (config.naive) {
                    // Baseline without coordination: just go in.
                    inside[a.node] = true;
                    entryIndex[a.node] = report.entries.size();
                    report.entries.push_back({a.node, 0, now, 0});
                    log(a.node, "==> ENTERS critical section (no coordination)");
                    sched.at(now + config.csDuration.count(), Action::Kind::Release, a.node);
                    break;
                }
                auto out = nodes.at(a.node).request();
                if (out.ok()) {
                    log(a.node, "wants the critical section (request ts=" +
                                    std::to_string(nodes.at(a.node).requestTimestamp()) + ")");
                    send(out.value());
                }
                break;
            }
            case Action::Kind::Release: {
                if (dead.count(a.node) != 0 || !inside[a.node]) {
                    break;
                }
                inside[a.node] = false;
                report.entries[entryIndex[a.node]].exitMs = now;
                log(a.node, "<== EXITS critical section");
                if (!config.naive) {
                    auto out = nodes.at(a.node).release();
                    if (out.ok()) {
                        send(out.value());
                    }
                }
                if (issued[a.node] < config.requestsPerNode) {
                    sched.at(now + 1 + sched.randomBelow(config.maxThink.count()), Action::Kind::Request, a.node);
                }
                break;
            }
            case Action::Kind::Deliver: {
                if (config.naive) {
                    break;
                }
                auto decoded = psx::sync::decodeMutexMessage(a.bytes);
                if (!decoded.ok()) {
                    break;
                }
                const MutexMessage& m = decoded.value();
                if (dead.count(m.to) != 0 || dead.count(m.from) != 0) {
                    break;
                }
                auto out = nodes.at(m.to).onMessage(m);
                if (!out.ok()) {
                    log(m.to, std::string("rejected ") + name(m.type) + ": " + out.error().message());
                    break;
                }
                send(out.value());
                break;
            }
            case Action::Kind::Kill: {
                dead.insert(a.node);
                report.events.push_back({now, a.node, 0,
                                         std::string("!!! CRASHED (kill -9)") +
                                             (inside[a.node] ? " while INSIDE the critical section" : "") + " !!!"});
                inside[a.node] = false;
                break;
            }
            case Action::Kind::Detect: {
                // The failure detector (lease expiry) reports the crash; every
                // survivor stops waiting for the dead node's REPLY.
                for (auto& [id, node] : nodes) {
                    if (dead.count(id) == 0) {
                        node.removePeer(a.node);
                    }
                }
                report.events.push_back(
                    {now, 0, 0, "failure detector: node " + std::to_string(a.node) + " declared dead, removed"});
                break;
            }
            default:
                break;
        }
        check();
    }

    // Liveness is judged on survivors: every request a live node made must
    // have been served. (A crashed node's unfinished request is not owed.)
    for (PeerId id = 1; id <= n; ++id) {
        if (dead.count(id) == 0) {
            report.requestsExpected += issued[id];
        }
    }
    for (const auto& e : report.entries) {
        if (dead.count(e.node) == 0 && e.exitMs != 0) {
            ++report.requestsServed;
        }
    }
    return report;
}

} // namespace psx::sim
