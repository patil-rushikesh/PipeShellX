#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

// Executes the `pipeshellx snapshot` subcommand:
//   pipeshellx snapshot <file.jsonl> [--latest] [--json]
//   pipeshellx snapshot dump <file.jsonl> [--latest] [--json]
int runSnapshotCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
