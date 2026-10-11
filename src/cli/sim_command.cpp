#include "psx/cli/sim_command.hpp"

#include "psx/sim/simulation.hpp"

#include <charconv>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace psx::cli {

namespace {

const char* kUsage = R"(Usage: pipeshellx sim election [options]
       pipeshellx sim mutex    [options]

Run PipeShellX's coordination algorithms on a simulated network (random
per-message delay, so messages arrive out of order), print the timeline with
Lamport timestamps / election epochs, and check the algorithm's invariants.

Common options:
  --nodes N            number of nodes (1-64, default 5 election / 3 mutex)
  --seed S             random seed; the same seed replays the same run
  --delay MIN-MAX      per-message network delay in ms (default 5-60)
  --quiet              summary only, no timeline

election (Bully + leader beacon):
  --kill-leader-at MS  crash the current leader at MS
  --revive-at MS       restart the crashed node (fresh state) at MS
  --duration MS        simulated time to run (default 6000)

mutex (Ricart-Agrawala):
  --requests N         critical-section entries per node (default 2)
  --cs MS              time spent inside the critical section (default 40)
  --naive              no coordination: shows the overlap RA prevents
  --kill NODE@MS       crash NODE at MS
  --detect MS          failure-detector delay before removing it (default 500)

Exit status: 0 invariants held, 1 an invariant was violated, 2 usage error.
)";

constexpr std::uint32_t kMaxNodes = 64;
constexpr std::int64_t kMaxMs = 10 * 60 * 1000;

// Strict unsigned parse: the whole token must be digits, in range.
std::optional<std::int64_t> parseNumber(std::string_view s, std::int64_t lo, std::int64_t hi) {
    std::int64_t v = 0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (s.empty() || ec != std::errc{} || ptr != end || v < lo || v > hi) {
        return std::nullopt;
    }
    return v;
}

void printTimeline(const std::vector<psx::sim::Event>& events, const char* clockLabel, std::ostream& out) {
    out << "  " << std::left << std::setw(9) << "time" << std::setw(7) << "node" << std::setw(8) << clockLabel
        << "event\n";
    for (const auto& e : events) {
        out << "  " << std::left << std::setw(9) << ("+" + std::to_string(e.timeMs) + "ms") << std::setw(7)
            << (e.node == 0 ? std::string("-") : "n" + std::to_string(e.node)) << std::setw(8)
            << (e.lamport == 0 ? std::string("-") : std::to_string(e.lamport)) << e.text << "\n";
    }
}

const char* mark(bool ok) {
    return ok ? "[ok]  " : "[FAIL]";
}

} // namespace

int runSimCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        (args.empty() ? err : out) << kUsage;
        return args.empty() ? 2 : 0;
    }
    const std::string mode = args[0];
    if (mode != "election" && mode != "mutex") {
        err << "pipeshellx sim: unknown simulation '" << mode << "' (expected election or mutex)\n";
        return 2;
    }
    const bool election = mode == "election";

    std::optional<std::uint32_t> nodes;
    psx::sim::NetworkConfig net;
    bool quiet = false;
    psx::sim::ElectionConfig ec;
    psx::sim::MutexConfig mc;

    auto usageError = [&](const std::string& what) {
        err << "pipeshellx sim " << mode << ": " << what << "\n";
        return 2;
    };

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                return std::nullopt;
            }
            return args[++i];
        };
        auto ms = [&](const std::string& flag) -> std::optional<std::chrono::milliseconds> {
            auto v = value();
            if (!v) {
                return std::nullopt;
            }
            auto n = parseNumber(*v, 0, kMaxMs);
            if (!n) {
                err << "pipeshellx sim " << mode << ": " << flag << " expects milliseconds (0-" << kMaxMs << ")\n";
                return std::nullopt;
            }
            return std::chrono::milliseconds(*n);
        };

        if (a == "--help" || a == "-h") {
            out << kUsage;
            return 0;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--nodes") {
            auto v = value();
            auto n = v ? parseNumber(*v, 1, kMaxNodes) : std::nullopt;
            if (!n) {
                return usageError("--nodes expects 1-64");
            }
            nodes = static_cast<std::uint32_t>(*n);
        } else if (a == "--seed") {
            auto v = value();
            auto n = v ? parseNumber(*v, 0, 0xFFFFFFFFLL) : std::nullopt;
            if (!n) {
                return usageError("--seed expects a 32-bit unsigned number");
            }
            net.seed = static_cast<std::uint32_t>(*n);
        } else if (a == "--delay") {
            auto v = value();
            const auto dash = v ? v->find('-') : std::string::npos;
            std::optional<std::int64_t> lo, hi;
            if (dash != std::string::npos) {
                lo = parseNumber(std::string_view(*v).substr(0, dash), 0, 60000);
                hi = parseNumber(std::string_view(*v).substr(dash + 1), 0, 60000);
            }
            if (!lo || !hi || *lo > *hi) {
                return usageError("--delay expects MIN-MAX milliseconds, MIN <= MAX <= 60000");
            }
            net.minDelay = std::chrono::milliseconds(*lo);
            net.maxDelay = std::chrono::milliseconds(*hi);
        } else if (election && a == "--kill-leader-at") {
            auto v = ms(a);
            if (!v) {
                return 2;
            }
            ec.killLeaderAt = *v;
        } else if (election && a == "--revive-at") {
            auto v = ms(a);
            if (!v) {
                return 2;
            }
            ec.reviveAt = *v;
        } else if (election && a == "--duration") {
            auto v = ms(a);
            if (!v) {
                return 2;
            }
            ec.duration = *v;
        } else if (!election && a == "--requests") {
            auto v = value();
            auto n = v ? parseNumber(*v, 1, 100) : std::nullopt;
            if (!n) {
                return usageError("--requests expects 1-100");
            }
            mc.requestsPerNode = static_cast<std::uint32_t>(*n);
        } else if (!election && a == "--cs") {
            auto v = ms(a);
            if (!v) {
                return 2;
            }
            mc.csDuration = *v;
        } else if (!election && a == "--naive") {
            mc.naive = true;
        } else if (!election && a == "--kill") {
            auto v = value();
            const auto at = v ? v->find('@') : std::string::npos;
            if (at == std::string::npos) {
                return usageError("--kill expects NODE@MS, e.g. 2@100");
            }
            auto node = parseNumber(std::string_view(*v).substr(0, at), 1, kMaxNodes);
            auto when = parseNumber(std::string_view(*v).substr(at + 1), 0, kMaxMs);
            if (!node || !when) {
                return usageError("--kill expects NODE@MS, e.g. 2@100");
            }
            mc.killNode = static_cast<std::uint32_t>(*node);
            mc.killAt = std::chrono::milliseconds(*when);
        } else if (!election && a == "--detect") {
            auto v = ms(a);
            if (!v) {
                return 2;
            }
            mc.detectAfter = *v;
        } else {
            return usageError("unknown option: " + a);
        }
    }

    const auto delayText = std::to_string(net.minDelay.count()) + "-" + std::to_string(net.maxDelay.count()) + " ms";

    if (election) {
        ec.nodes = nodes.value_or(5);
        ec.network = net;
        if (ec.reviveAt && !ec.killLeaderAt) {
            return usageError("--revive-at needs --kill-leader-at");
        }
        const auto r = psx::sim::runElection(ec);
        out << "Bully leader election with leader beacon: " << ec.nodes << " nodes, seed " << net.seed
            << ", network delay " << delayText << "\n\n";
        if (!quiet) {
            printTimeline(r.events, "epoch", out);
            out << "\n";
        }
        out << "Summary\n";
        out << "  messages sent (including leader beacons): " << r.messagesSent << "\n";
        out << "  elections started: " << r.electionsStarted << "\n";
        if (r.killedNode) {
            out << "  crashed node: n" << *r.killedNode << "\n";
            if (r.recoveredAtMs) {
                out << "  recovered at +" << *r.recoveredAtMs << "ms (" << (*r.recoveredAtMs - ec.killLeaderAt->count())
                    << " ms after the crash)\n";
            }
        }
        out << "  " << mark(r.agreed) << " every live node agrees the leader is n" << r.expectedLeader
            << " (the highest live id)\n";
        return r.agreed ? 0 : 1;
    }

    mc.nodes = nodes.value_or(3);
    mc.network = net;
    if (mc.killNode && *mc.killNode > mc.nodes) {
        return usageError("--kill names a node that does not exist");
    }
    const auto r = psx::sim::runMutex(mc);
    out << (mc.naive ? "NO coordination (naive baseline)" : "Ricart-Agrawala mutual exclusion") << ": " << mc.nodes
        << " nodes, " << mc.requestsPerNode << " entries each, seed " << net.seed << ", network delay " << delayText
        << "\n\n";
    if (!quiet) {
        printTimeline(r.events, "lamport", out);
        out << "\n";
    }
    const bool safe = r.violations == 0;
    const bool live = r.requestsServed == r.requestsExpected;
    out << "Summary\n";
    out << "  messages sent: " << r.messagesSent;
    if (!mc.naive && !mc.killNode && r.requestsServed > 0) {
        out << " (" << (r.messagesSent / r.requestsServed) << " per entry; theory 2(N-1) = " << 2 * (mc.nodes - 1)
            << ")";
    }
    out << "\n";
    out << "  " << mark(safe) << " safety: at most one node inside at a time (max seen: " << r.maxConcurrentHolders
        << ", violations: " << r.violations << ")\n";
    out << "  " << mark(live) << " liveness: " << r.requestsServed << "/" << r.requestsExpected
        << " requests from live nodes served\n";
    if (!mc.naive) {
        out << "  " << mark(r.grantedInTimestampOrder)
            << " fairness: concurrent requests granted in (lamport ts, id) order\n";
    }
    return safe && live && (mc.naive || r.grantedInTimestampOrder) ? 0 : 1;
}

} // namespace psx::cli
