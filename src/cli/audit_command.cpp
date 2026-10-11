#include "psx/cli/audit_command.hpp"

#include "psx/audit/audit_log.hpp"

#include <algorithm>

namespace psx::cli {

namespace {

const char* kUsage = R"(Usage: pipeshellx audit verify FILE [--expect-head HASH]

Verify the tamper-evident hash chain of a --audit-log file. Every record names
the SHA-256 of the previous one, so editing, deleting, inserting or reordering
records breaks the chain at the first altered record.

  --expect-head HASH   also require the chain to end at HASH. Save the head
                       printed by a previous verify somewhere the log's writer
                       cannot change; this is what detects records cut off the
                       end or a chain rewritten wholesale.

Exit status: 0 intact, 1 broken or head mismatch, 2 usage error.
)";

bool isLowerHex64(const std::string& s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace

int runAuditCommand(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        err << kUsage;
        return 2;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        out << kUsage;
        return 0;
    }
    if (args[0] != "verify") {
        err << "pipeshellx audit: unknown subcommand '" << args[0] << "' (expected verify)\n";
        return 2;
    }
    std::string path;
    std::string expectHead;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--expect-head") {
            if (i + 1 >= args.size() || !isLowerHex64(args[i + 1])) {
                err << "pipeshellx audit verify: --expect-head needs a 64-character lower-case hex hash\n";
                return 2;
            }
            expectHead = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            err << "pipeshellx audit verify: unknown option " << args[i] << "\n";
            return 2;
        } else if (path.empty()) {
            path = args[i];
        } else {
            err << "pipeshellx audit verify: unexpected argument " << args[i] << "\n";
            return 2;
        }
    }
    if (path.empty()) {
        err << "pipeshellx audit verify: missing FILE\n";
        return 2;
    }

    auto result = psx::audit::verifyAuditLog(path);
    if (!result.ok()) {
        err << "pipeshellx audit verify: cannot read " << path << "\n";
        return 2;
    }
    const auto& r = result.value();
    if (!r.intact) {
        out << "BROKEN at line " << r.brokenLine << ": " << r.reason << "\n";
        out << "  " << r.records << " record(s) before it verified\n";
        return 1;
    }
    out << "OK: hash chain intact, " << r.records << " chained record(s)";
    if (r.legacyRecords > 0) {
        out << " after " << r.legacyRecords << " unchained legacy record(s)";
    }
    out << "\n";
    if (r.head.empty()) {
        out << "  (no chained records yet)\n";
    } else {
        out << "  head: " << r.head << "\n";
        out << "  keep this head somewhere the log's writer cannot modify, then use --expect-head\n";
    }
    if (!expectHead.empty() && expectHead != r.head) {
        out << "HEAD MISMATCH: expected " << expectHead << "\n"
            << "  records were removed from the end, or the chain was rewritten\n";
        return 1;
    }
    return 0;
}

} // namespace psx::cli
