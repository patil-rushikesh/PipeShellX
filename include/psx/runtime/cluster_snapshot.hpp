#pragma once

#include "psx/json/json.hpp"
#include "psx/result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace psx::runtime {

// One node's state as of the moment it was record()-ed into a
// ClusterSnapshot. See docs/ds-project/02-cluster-snapshot.md for the
// simplifications versus a full Chandy-Lamport snapshot (this captures
// local state only, no in-flight channel bytes).
struct NodeSnapshot {
    std::string host;
    std::string stageId;         // empty if nothing running on this host right now
    std::string status;          // e.g. "running", "exited", "connecting", "lost"
    int exitCode = 0;            // meaningful only when status == "exited"
    std::uint64_t lamportTs = 0; // 0 if unknown / peer has no LamportClock wired in yet

    bool operator==(const NodeSnapshot& other) const {
        return host == other.host && stageId == other.stageId && status == other.status &&
               exitCode == other.exitCode && lamportTs == other.lamportTs;
    }
};

// A point-in-time capture across every tracked node in a run. Passive: the
// caller decides when "now" is and calls record() once per host, then
// serializes. This class never polls the network itself.
class ClusterSnapshot {
public:
    explicit ClusterSnapshot(std::string runId);
    ClusterSnapshot(std::string runId, std::int64_t tsEpochMs);

    // Records one node's state. Call once per host per capture round.
    void record(NodeSnapshot node) { nodes_.push_back(std::move(node)); }

    const std::vector<NodeSnapshot>& nodes() const noexcept { return nodes_; }
    const std::string& runId() const noexcept { return runId_; }
    std::int64_t timestampEpochMs() const noexcept { return tsEpochMs_; }

    // One JSON object, no trailing newline.
    std::string toJsonLine() const;

    // Appends toJsonLine() + '\n' to `path`, creating missing parent directories.
    bool appendToFile(const std::string& path) const;

    // Parses a snapshot from a JSON line string.
    static psx::Result<ClusterSnapshot> fromJsonLine(std::string_view line);

    // Reads all snapshots from a JSONL file.
    static psx::Result<std::vector<ClusterSnapshot>> readFromFile(const std::string& path);

    // Formats this snapshot as an aligned table suitable for CLI inspection.
    std::string formatTable() const;

private:
    static std::int64_t nowMillis() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    std::string runId_;
    std::int64_t tsEpochMs_ = 0;
    std::vector<NodeSnapshot> nodes_;
};

} // namespace psx::runtime
