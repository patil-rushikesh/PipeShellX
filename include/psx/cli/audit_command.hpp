#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace psx::cli {

// `pipeshellx audit verify FILE [--expect-head HASH]`: walk the audit log's
// hash chain and report the first broken record. Exit 0 intact, 1 broken or
// head mismatch, 2 usage error or unreadable file.
int runAuditCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace psx::cli
