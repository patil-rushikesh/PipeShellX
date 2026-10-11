#include "psx/audit/audit_log.hpp"

#include "psx/audit/sha256.hpp"
#include "psx/json/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace psx::audit {

namespace {

const std::string kGenesis(64, '0');
constexpr std::string_view kHashKey = ",\"hash\":\""; // 9 bytes
constexpr std::string_view kPrevKey = ",\"prev\":\""; // 9 bytes
constexpr std::size_t kHashSuffix = 9 + 64 + 2;       // ,"hash":"<64>"}
constexpr std::size_t kPrevSuffix = 9 + 64 + 1;       // ,"prev":"<64>"
// Longest record we will scan back for when reopening; a real record is a few
// hundred bytes (commands and error strings included).
constexpr std::size_t kMaxTailScan = 1U << 20U;

std::int64_t nowMillis() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool isLowerHex64(std::string_view s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Splits a chained record into (signed prefix, prev, hash). The layout is
// fixed at the end of the line, and json::quote escapes every '"' inside a
// field value, so a field's content cannot imitate this suffix.
struct Chained {
    std::string_view prefix;
    std::string_view prev;
    std::string_view hash;
};
bool parseChained(std::string_view line, Chained& out) {
    if (line.size() < kHashSuffix + kPrevSuffix || line.substr(line.size() - 2) != "\"}") {
        return false;
    }
    const std::size_t hashAt = line.size() - kHashSuffix;
    if (line.substr(hashAt, kHashKey.size()) != kHashKey) {
        return false;
    }
    out.prefix = line.substr(0, hashAt);
    out.hash = line.substr(hashAt + kHashKey.size(), 64);
    const std::size_t prevAt = out.prefix.size() - kPrevSuffix;
    if (out.prefix.substr(prevAt, kPrevKey.size()) != kPrevKey || out.prefix.back() != '"') {
        return false;
    }
    out.prev = out.prefix.substr(prevAt + kPrevKey.size(), 64);
    return isLowerHex64(out.hash) && isLowerHex64(out.prev);
}

// The hash a successor must name as its `prev`: the record's own hash if it
// is chained, otherwise the hash of its raw bytes (legacy/partial lines).
std::string linkHash(std::string_view line) {
    Chained c;
    return parseChained(line, c) ? std::string(c.hash) : sha256Hex(line);
}

// Reads the file's last line (without its newline) and whether the file ends
// with '\n'. Scans backwards so a large log is not read in full.
struct Tail {
    bool empty = true;
    bool endsWithNewline = true;
    bool tooLong = false;
    std::string lastLine;
};
Tail readTail(const std::string& path) {
    Tail tail;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return tail;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(std::max<std::streamoff>(in.tellg(), 0));
    if (size == 0) {
        return tail;
    }
    tail.empty = false;
    const std::size_t window = std::min(size, kMaxTailScan + 1);
    std::string buf(window, '\0');
    in.seekg(static_cast<std::streamoff>(size - window));
    in.read(buf.data(), static_cast<std::streamsize>(window));
    tail.endsWithNewline = buf.back() == '\n';
    std::string_view view(buf);
    if (tail.endsWithNewline) {
        view.remove_suffix(1);
    }
    const auto nl = view.rfind('\n');
    if (nl == std::string_view::npos && window < size) {
        tail.tooLong = true; // the last record is longer than we are willing to scan
        return tail;
    }
    tail.lastLine = std::string(nl == std::string_view::npos ? view : view.substr(nl + 1));
    return tail;
}

} // namespace

AuditLog::AuditLog(const std::string& path) : prev_(kGenesis) {
    std::error_code ignored;
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    const bool existed = std::filesystem::exists(file, ignored);
    const Tail tail = existed ? readTail(path) : Tail{};
    out_.open(path, std::ios::app | std::ios::binary);
    if (!out_) {
        return;
    }
    if (!existed) {
        // Audit records contain the commands that were run, which can include
        // secrets passed as arguments: a new log is owner-only.
        std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ignored);
    }
    if (tail.empty) {
        return;
    }
    if (tail.tooLong) {
        // Cannot recover the link; start a new segment. verify will report the
        // break at our first record, which is the honest outcome.
        prev_ = kGenesis;
    } else {
        prev_ = linkHash(tail.lastLine);
    }
    if (!tail.endsWithNewline) {
        // A crash left a partial record. Terminate it so our record is not
        // glued onto it; we chain to its raw bytes, so the partial record stays
        // visible to verify as unchained data rather than vanishing.
        out_ << '\n';
    }
}

void AuditLog::writeLine(const std::string& body) {
    if (!out_) {
        return;
    }
    std::ostringstream prefix;
    prefix << "{\"ts_ms\":" << nowMillis() << ',' << body << kPrevKey << prev_ << '"';
    const std::string signedPart = prefix.str();
    const std::string hash = sha256Hex(signedPart);
    out_ << signedPart << kHashKey << hash << "\"}\n";
    out_.flush(); // durability over throughput: an audit line must survive a crash
    if (out_) {
        prev_ = hash;
    }
}

void AuditLog::runStarted(const std::string& runId, const std::string& command, std::size_t hostCount) {
    std::ostringstream body;
    body << "\"event\":\"run_started\",\"run_id\":" << json::quote(runId) << ",\"command\":" << json::quote(command)
         << ",\"hosts\":" << hostCount;
    writeLine(body.str());
}

void AuditLog::stageFinished(const std::string& runId, const StageRecord& record) {
    std::ostringstream body;
    body << "\"event\":\"stage_finished\",\"run_id\":" << json::quote(runId)
         << ",\"stage\":" << json::quote(record.stageId) << ",\"host\":" << json::quote(record.host)
         << ",\"exit_code\":" << record.exitCode << ",\"attempts\":" << record.attempts
         << ",\"timed_out\":" << json::boolean(record.timedOut) << ",\"cancelled\":" << json::boolean(record.cancelled)
         << ",\"aborted\":" << json::boolean(record.aborted) << ",\"dropped_bytes\":" << record.droppedBytes
         << ",\"error\":" << json::quote(record.error);
    writeLine(body.str());
}

void AuditLog::runFinished(const std::string& runId, const RunRecord& record) {
    std::ostringstream body;
    body << "\"event\":\"run_finished\",\"run_id\":" << json::quote(runId) << ",\"total\":" << record.total
         << ",\"succeeded\":" << record.succeeded << ",\"failed\":" << record.failed
         << ",\"cancelled\":" << json::boolean(record.cancelled) << ",\"exit_code\":" << record.exitCode;
    writeLine(body.str());
}

psx::Result<VerifyReport> verifyAuditLog(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return psx::Error{psx::ErrorClass::NotFound, 0, "audit verify: cannot open log"};
    }
    VerifyReport report;
    std::string expectedPrev = kGenesis;
    bool chainStarted = false;
    std::size_t lineNo = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        Chained c;
        if (!parseChained(line, c)) {
            if (chainStarted) {
                report.brokenLine = lineNo;
                report.reason = "record is not chained (edited, inserted, or a partial write)";
                return report;
            }
            ++report.legacyRecords; // pre-chaining history: tolerated as a prefix
            expectedPrev = sha256Hex(line);
            continue;
        }
        chainStarted = true;
        if (c.prev != expectedPrev) {
            report.brokenLine = lineNo;
            report.reason = "prev hash does not match the previous record (deleted, inserted or reordered)";
            return report;
        }
        if (sha256Hex(c.prefix) != c.hash) {
            report.brokenLine = lineNo;
            report.reason = "record content does not match its hash (modified)";
            return report;
        }
        expectedPrev = std::string(c.hash);
        ++report.records;
    }
    report.intact = true;
    report.head = chainStarted ? expectedPrev : std::string();
    return report;
}

} // namespace psx::audit
