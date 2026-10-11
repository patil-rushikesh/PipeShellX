#include "psx/runtime/cluster_snapshot.hpp"

#include "psx/json/reader.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

namespace psx::runtime {

namespace {

psx::Error malformed(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}

// One snapshot line is a few hundred bytes per node; 4 MiB is far above any
// real fleet (~20k nodes) yet stops one unterminated multi-GB "line" in a
// shared or corrupted file from exhausting memory in `snapshot dump`.
constexpr std::size_t kMaxLineBytes = 4U * 1024U * 1024U;

std::string stringField(const psx::json::Value& object, std::string_view key) {
    const psx::json::Value* v = object.find(key);
    const std::string* s = v != nullptr ? v->asString() : nullptr;
    return s != nullptr ? *s : std::string();
}

std::optional<std::int64_t> intField(const psx::json::Value& object, std::string_view key) {
    const psx::json::Value* v = object.find(key);
    const std::int64_t* i = v != nullptr ? v->asInt() : nullptr;
    return i != nullptr ? std::optional<std::int64_t>(*i) : std::nullopt;
}

// formatTable() output goes to a terminal. Snapshot files can be shared, so a
// host or status string must not be able to smuggle ANSI escape sequences
// (cursor moves, title changes, hidden text) into the operator's terminal.
std::string printable(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x20 || u == 0x7F ? '?' : c);
    }
    return out;
}

} // namespace

ClusterSnapshot::ClusterSnapshot(std::string runId)
    : runId_(std::move(runId)), tsEpochMs_(nowMillis()) {}

ClusterSnapshot::ClusterSnapshot(std::string runId, std::int64_t tsEpochMs)
    : runId_(std::move(runId)), tsEpochMs_(tsEpochMs) {}

std::string ClusterSnapshot::toJsonLine() const {
    std::ostringstream body;
    body << "{\"type\":\"cluster_snapshot\",\"run_id\":" << json::quote(runId_)
         << ",\"ts_epoch_ms\":" << tsEpochMs_ << ",\"nodes\":[";
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const NodeSnapshot& n = nodes_[i];
        if (i != 0) {
            body << ',';
        }
        body << "{\"host\":" << json::quote(n.host) << ",\"stage_id\":" << json::quote(n.stageId)
             << ",\"status\":" << json::quote(n.status) << ",\"exit_code\":" << n.exitCode
             << ",\"lamport_ts\":" << n.lamportTs << "}";
    }
    body << "]}";
    return body.str();
}

bool ClusterSnapshot::appendToFile(const std::string& path) const {
    std::error_code ignored;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    const bool existed = std::filesystem::exists(file, ignored);
    std::ofstream out(path, std::ios::app);
    if (!out) {
        return false;
    }
    if (!existed) {
        // Snapshots list every host in the run and what it was doing: keep a
        // freshly created file owner-only, like a private log, rather than
        // inheriting a world-readable umask. An existing file keeps whatever
        // mode its owner chose.
        std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ignored);
    }
    out << toJsonLine() << '\n';
    return static_cast<bool>(out);
}

psx::Result<ClusterSnapshot> ClusterSnapshot::fromJsonLine(std::string_view line) {
    if (line.size() > kMaxLineBytes) {
        return malformed("snapshot line exceeds size limit");
    }
    auto doc = psx::json::parse(line, {.maxBytes = kMaxLineBytes, .maxDepth = 8});
    if (!doc.ok()) {
        return malformed("snapshot line is not valid JSON");
    }
    const psx::json::Value& root = doc.value();
    if (root.asObject() == nullptr || stringField(root, "type") != "cluster_snapshot") {
        return malformed("not a cluster_snapshot line");
    }
    const psx::json::Value* runIdValue = root.find("run_id");
    if (runIdValue == nullptr || runIdValue->asString() == nullptr) {
        return malformed("missing run_id");
    }
    ClusterSnapshot snapshot(*runIdValue->asString(), intField(root, "ts_epoch_ms").value_or(0));

    const psx::json::Value* nodesValue = root.find("nodes");
    if (nodesValue == nullptr) {
        return snapshot;
    }
    const psx::json::Array* nodes = nodesValue->asArray();
    if (nodes == nullptr) {
        return malformed("nodes is not an array");
    }
    for (const psx::json::Value& item : *nodes) {
        if (item.asObject() == nullptr) {
            return malformed("node entry is not an object");
        }
        NodeSnapshot node;
        node.host = stringField(item, "host");
        node.stageId = stringField(item, "stage_id");
        node.status = stringField(item, "status");
        const auto exitCode = intField(item, "exit_code").value_or(0);
        if (exitCode < std::numeric_limits<int>::min() || exitCode > std::numeric_limits<int>::max()) {
            return malformed("exit_code out of range");
        }
        node.exitCode = static_cast<int>(exitCode);
        // Lamport timestamps are unsigned. A negative value is corrupt data;
        // casting it would turn -1 into 18446744073709551615 and reorder events.
        const auto lamport = intField(item, "lamport_ts").value_or(0);
        if (lamport < 0) {
            return malformed("lamport_ts is negative");
        }
        node.lamportTs = static_cast<std::uint64_t>(lamport);
        snapshot.record(std::move(node));
    }
    return snapshot;
}

psx::Result<std::vector<ClusterSnapshot>> ClusterSnapshot::readFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return malformed("cannot open snapshot file");
    }
    std::vector<ClusterSnapshot> result;
    std::string line;
    char c = 0;
    bool overlong = false;
    // Read with a hard per-line cap instead of std::getline, which would
    // buffer an arbitrarily long line in memory before we could reject it.
    auto finishLine = [&] {
        if (!overlong && !line.empty()) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            auto parsed = fromJsonLine(line);
            if (parsed.ok()) {
                result.push_back(std::move(parsed.value()));
            }
        }
        line.clear();
        overlong = false;
    };
    while (in.get(c)) {
        if (c == '\n') {
            finishLine();
        } else if (!overlong) {
            if (line.size() >= kMaxLineBytes) {
                overlong = true; // skip the rest of this line without storing it
                line.clear();
            } else {
                line.push_back(c);
            }
        }
    }
    finishLine();
    return result;
}

std::string ClusterSnapshot::formatTable() const {
    std::ostringstream out;
    out << "================================================================================\n";
    out << "Cluster Snapshot [Run: " << printable(runId_) << " | Timestamp: " << tsEpochMs_ << " ms]\n";
    out << "================================================================================\n";
    out << std::left << std::setw(24) << "HOST"
        << std::left << std::setw(16) << "STAGE"
        << std::left << std::setw(14) << "STATUS"
        << std::left << std::setw(10) << "EXIT"
        << "LAMPORT_TS\n";
    out << "--------------------------------------------------------------------------------\n";
    for (const auto& node : nodes_) {
        std::string exitStr = (node.status == "exited") ? std::to_string(node.exitCode) : "-";
        out << std::left << std::setw(24) << printable(node.host) << std::left << std::setw(16)
            << (node.stageId.empty() ? std::string("-") : printable(node.stageId)) << std::left << std::setw(14)
            << printable(node.status) << std::left << std::setw(10) << exitStr << node.lamportTs << "\n";
    }
    out << "================================================================================\n";
    return out.str();
}

} // namespace psx::runtime
