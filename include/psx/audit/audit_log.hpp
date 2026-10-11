#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

#include "psx/result.hpp"

namespace psx::audit {

// One stage's outcome, as recorded in the audit trail.
struct StageRecord {
    std::string host;
    std::string stageId;
    int exitCode = 0;
    int attempts = 1;
    bool timedOut = false;
    bool cancelled = false;
    bool aborted = false;
    std::uint64_t droppedBytes = 0;
    std::string error; // classified error message (may be empty)
};

// A run's aggregate outcome.
struct RunRecord {
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    int exitCode = 0;
};

// Appends one JSON object per line (JSONL): a run_started, one stage_finished
// per host, and a run_finished, all sharing the run's run_id and each carrying
// an epoch-millisecond timestamp. Opens the file in append mode (creating parent
// directories); an open failure is silent — ok() reports it — so an unwritable
// audit path degrades to "no audit", never aborting the run.
//
// Hash chain (tamper evidence). Every record ends with
//     ..., "prev":"<sha256 of the previous record>", "hash":"<sha256 of this record>"}
// where `hash` covers every byte of the line before `,"hash":"`, prev included.
// Changing, deleting, inserting or reordering any record breaks every later
// link, and verifyAuditLog() reports the first broken one. The chain continues
// across reopen (the previous record's hash is read from the file's tail).
//
// What a hash chain does NOT do, stated so nobody over-claims it: someone who
// can write the file can rewrite the whole chain from the edit onward, or cut
// records off the end, and the chain still verifies. Detecting that needs the
// head hash kept somewhere the writer cannot reach (verify --expect-head), or
// signatures. Two processes appending to one file concurrently fork the chain;
// verify reports that as a break rather than hiding it.
class AuditLog {
public:
    explicit AuditLog(const std::string& path);

    bool ok() const noexcept { return static_cast<bool>(out_); }
    // Hash of the last record written (or found at open): the chain's head.
    const std::string& head() const noexcept { return prev_; }

    void runStarted(const std::string& runId, const std::string& command, std::size_t hostCount);
    void stageFinished(const std::string& runId, const StageRecord& record);
    void runFinished(const std::string& runId, const RunRecord& record);

private:
    void writeLine(const std::string& body);

    std::ofstream out_;
    std::string prev_; // hash of the previous record; 64 zeros for a new log
};

// Result of walking a log's hash chain.
struct VerifyReport {
    bool intact = false;
    std::size_t records = 0;       // chained records checked
    std::size_t legacyRecords = 0; // unchained records before the first chained one
    std::size_t brokenLine = 0;    // 1-based line of the first failure (0 = none)
    std::string reason;            // why it failed
    std::string head;              // hash of the last record (anchor this)
};

// Walks the file's chain. Returns an error only if the file cannot be read.
// Unchained lines are accepted only as a prefix (logs written before chaining
// existed); an unchained line after the chain has started is a break.
psx::Result<VerifyReport> verifyAuditLog(const std::string& path);

} // namespace psx::audit
