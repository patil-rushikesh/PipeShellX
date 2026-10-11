#pragma once

// Discrete-event simulators for the coordination algorithms
// (psx::election::BullyNode and psx::sync::RicartAgrawala).
//
// Why a simulator: the algorithms are sans-IO state machines, so they can be
// driven by a virtual network that injects exactly the conditions a real
// network produces only by luck -- random per-message delay (so messages
// arrive out of order), crashed nodes, restarts -- with a fixed seed so every
// run is reproducible. Every message is passed through the real wire codec
// (encode -> bytes -> decode) on its way, so the simulator exercises the same
// bytes a socket transport would carry.
//
// The simulators check the algorithms' invariants continuously and report
// them; `pipeshellx sim` prints the timeline for demos, and the unit tests run
// hundreds of seeds as property tests.

#include "psx/election/bully.hpp"
#include "psx/sync/ricart_agrawala.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace psx::sim {

struct NetworkConfig {
    std::uint32_t seed = 1;
    std::chrono::milliseconds minDelay{5};
    std::chrono::milliseconds maxDelay{60}; // delays vary -> messages reorder
};

// One line of the timeline. `lamport` is the acting node's clock (mutex) or
// election epoch (election) at that moment; 0 when not meaningful.
struct Event {
    std::int64_t timeMs = 0;
    std::uint32_t node = 0;
    std::uint64_t lamport = 0;
    std::string text;
};

// ---------------------------------------------------------------- election

struct ElectionConfig {
    std::uint32_t nodes = 5;
    NetworkConfig network;
    psx::election::Timeouts timeouts;
    std::chrono::milliseconds duration{6000};
    std::optional<std::chrono::milliseconds> killLeaderAt; // crash whoever leads then
    std::optional<std::chrono::milliseconds> reviveAt;     // restart it, fresh state
    bool traceMessages = true;                             // log every send/deliver
};

struct ElectionReport {
    std::vector<Event> events;
    std::uint64_t messagesSent = 0;
    std::uint64_t electionsStarted = 0;
    std::optional<std::uint32_t> killedNode;
    std::optional<std::int64_t> recoveredAtMs; // first time all live nodes agreed after the kill
    // Final view per live node (index = node id - 1; nullopt = dead or no leader).
    std::vector<std::optional<std::uint32_t>> finalLeaders;
    std::uint32_t expectedLeader = 0; // highest live id
    bool agreed = false;              // every live node names expectedLeader, and it leads
};

ElectionReport runElection(const ElectionConfig& config);

// ------------------------------------------------------------------- mutex

struct MutexConfig {
    std::uint32_t nodes = 3;
    NetworkConfig network;
    std::uint32_t requestsPerNode = 2;
    std::chrono::milliseconds csDuration{40};   // time spent inside
    std::chrono::milliseconds maxThink{30};     // random gap between requests
    bool naive = false;                         // no protocol: just enter (baseline)
    std::optional<std::uint32_t> killNode;      // crash this node ...
    std::chrono::milliseconds killAt{0};        // ... at this time
    std::chrono::milliseconds detectAfter{500}; // failure detector delay
    bool traceMessages = true;
};

struct CriticalSectionEntry {
    std::uint32_t node = 0;
    std::uint64_t requestTs = 0;
    std::int64_t enterMs = 0;
    std::int64_t exitMs = 0;
};

struct MutexReport {
    std::vector<Event> events;
    std::vector<CriticalSectionEntry> entries;
    std::uint64_t messagesSent = 0;
    std::uint32_t maxConcurrentHolders = 0; // safety: must be <= 1 unless naive
    std::uint32_t violations = 0;           // moments with >1 holder
    std::uint32_t requestsExpected = 0;
    std::uint32_t requestsServed = 0;
    bool grantedInTimestampOrder = true; // fairness among concurrent waiters
};

MutexReport runMutex(const MutexConfig& config);

} // namespace psx::sim
