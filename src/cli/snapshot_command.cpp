#include "psx/cli/snapshot_command.hpp"
#include "psx/runtime/cluster_snapshot.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace psx::cli {

namespace {

const char* kUsage = R"(Usage: pipeshellx snapshot [dump] <path> [options]

Inspect and format point-in-time cluster snapshots recorded during stage execution.

Arguments:
  <path>      path to the JSONL snapshot file

Options:
  --latest    only display the most recent snapshot in the file
  --json      emit raw JSON lines instead of a formatted table
  --help, -h  show this help message and exit
)";

} // namespace

int runSnapshotCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << kUsage;
        return 2;
    }

    std::string path;
    bool latestOnly = false;
    bool jsonOutput = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            out << kUsage;
            return 0;
        } else if (arg == "dump") {
            continue; // optional subcommand verb
        } else if (arg == "--latest") {
            latestOnly = true;
        } else if (arg == "--json") {
            jsonOutput = true;
        } else if (!arg.empty() && arg[0] == '-') {
            err << "pipeshellx snapshot: unknown option: " << arg << "\n";
            return 2;
        } else {
            if (path.empty()) {
                path = arg;
            } else {
                err << "pipeshellx snapshot: unexpected positional argument: " << arg << "\n";
                return 2;
            }
        }
    }

    if (path.empty()) {
        err << "pipeshellx snapshot: missing path to snapshot file\n";
        return 2;
    }

    auto snapshots = psx::runtime::ClusterSnapshot::readFromFile(path);
    if (!snapshots.ok()) {
        err << "pipeshellx snapshot: error: " << snapshots.error().message() << " (" << path << ")\n";
        return 1;
    }

    if (snapshots.value().empty()) {
        err << "pipeshellx snapshot: file contains no valid snapshot records\n";
        return 1;
    }

    const auto& list = snapshots.value();
    if (latestOnly) {
        const auto& snap = list.back();
        if (jsonOutput) {
            out << snap.toJsonLine() << "\n";
        } else {
            out << snap.formatTable();
        }
    } else {
        for (const auto& snap : list) {
            if (jsonOutput) {
                out << snap.toJsonLine() << "\n";
            } else {
                out << snap.formatTable();
            }
        }
    }

    return 0;
}

} // namespace psx::cli
