# Phase 2 — Cluster Snapshot (simplified Chandy-Lamport)

## Goal

A point-in-time, consistent-ish record of what the whole cluster was doing:
which stages were running on which hosts, their status, and (once Phase 1
is wired in) the Lamport timestamp of the most recent event observed for
each. This fills the "Global State" syllabus topic without needing the full
marker-flooding Chandy-Lamport protocol, which needs channel-state capture
that PipeShellX's point-to-point streams don't currently expose cheaply.

Be upfront about the simplification in whatever report/README describes
this: a **real** Chandy-Lamport snapshot also records in-flight channel
messages (bytes sent but not yet received) so the snapshot is causally
consistent even under concurrent activity. This implementation instead
takes a **local-state-only** snapshot: it asks each tracked connection for
its current status at the moment `capture()` is called, with no channel
markers. That's a legitimate, common simplification (many real systems do
"best-effort" snapshots this way) but say so explicitly rather than
claiming full Chandy-Lamport semantics.

## What already exists (done, don't redo)

`include/psx/runtime/cluster_snapshot.hpp`:

```cpp
namespace psx::runtime {

struct NodeSnapshot {
    std::string host;
    std::string stageId;   // empty if nothing running on this host right now
    std::string status;    // e.g. "running", "exited", "connecting", "lost"
    int exitCode = 0;      // meaningful only when status == "exited"
    std::uint64_t lamportTs = 0; // 0 if Phase 1 isn't wired in yet, or unknown
};

// One point-in-time capture across every tracked node.
class ClusterSnapshot {
public:
    explicit ClusterSnapshot(std::string runId);

    // Record one node's state as of "now". Call this once per host each
    // time you want a fresh capture -- the class doesn't poll anything
    // itself, the caller decides when "now" is.
    void record(NodeSnapshot node);

    // Every record() call since construction or the last capture(), in the
    // order recorded.
    const std::vector<NodeSnapshot>& nodes() const noexcept;

    // Serializes the current set of records as one JSON object per line
    // pattern matching psx::audit::AuditLog's style (see audit_log.hpp):
    // {"type":"cluster_snapshot","run_id":"...","ts_epoch_ms":...,
    //  "nodes":[{"host":...,"stage_id":...,"status":...,"exit_code":...,
    //  "lamport_ts":...}, ...]}
    std::string toJsonLine() const;

    // Convenience: append toJsonLine() to a file, same append/create-parent-
    // dirs/never-throw behaviour as AuditLog (an unwritable path degrades
    // to returning false, never aborts the caller).
    bool appendToFile(const std::string& path) const;

private:
    std::string runId_;
    std::vector<NodeSnapshot> nodes_;
};

} // namespace psx::runtime
```

`tests/unit/runtime/test_cluster_snapshot.cpp` -- covers: empty snapshot
serializes to an empty `nodes` array, `record()` accumulates in call order,
`toJsonLine()` produces valid single-line JSON with the expected keys,
`appendToFile()` writes one line per call and creates missing parent
directories, and an unwritable path returns `false` without throwing.

## What's NOT done yet -- your task

### Task 1: pick the capture point

`ClusterSnapshot` is a passive recorder -- something has to call `record()`
for each host and then `appendToFile()`. The natural owner is whichever
object already has a live list of per-host state at a point in time. Two
candidates, in order of how much you already know about their internals
from this planning session:

- **`NativeController`** owns `Conn` per target and already tracks
  `HostResult`-shaped state (host, exit code, timedOut, cancelled, etc. --
  see the `HostResult` struct in `native_controller.hpp`). This is
  probably the easier integration: after `start()` is called and
  periodically while targets are still running (e.g. on your existing
  liveness/PING timer, if there is one, or a new timer), call `record()`
  for every target's current best-known status and then
  `appendToFile(...)`.
- **`DistributedRunner`** is the other candidate for the pipeline
  (multi-stage chain) case rather than the fan-out case.

Read the actual `.cpp` for whichever one you pick before wiring this in --
this planning session read the headers, not the implementations, so the
exact field names inside `Conn` are not guaranteed accurate. Don't guess;
open the file.

### Task 2: decide the trigger

Simplest and most defensible for a course project: trigger a snapshot
**on every PING/PONG liveness round** (the wire protocol already pings
every ~2 seconds per `docs/wire_protocol.md`), since that's already a
natural "checkpoint" in the system's own timing. Alternatively, add a CLI
flag (`--snapshot-interval`) if you want it configurable for a demo. Don't
overbuild this -- a fixed interval is fine for a course project; a
user-configurable one is a nice-to-have, not a requirement.

### Task 3: surface it

Add a small CLI subcommand or flag that dumps the latest snapshot file in a
readable form (host | stage | status | lamport_ts table) -- this is what
you'd actually show in a demo/viva. Reuse `docs/architecture.md`'s existing
CLI patterns (`src/cli/`) rather than inventing a new entry point style.

## What NOT to do

- Do not implement real marker-flooding Chandy-Lamport with channel-state
  capture. It's not worth the engineering time against the wire protocol's
  actual constraints (streams don't currently expose "bytes sent but not
  yet acked" in a form this layer can read), and the local-state-only
  version above is a defensible, clearly-labeled simplification for a
  course project.
- Do not make `ClusterSnapshot` poll the network itself. It's a pure data
  structure + serializer; something else decides *when* and *what* to
  record. Keeping it passive is what makes it easy to unit test (see the
  existing test file) and easy to reason about.
- Do not block the reactor thread doing file I/O on every single stage
  event. Batch: accumulate `record()` calls, then `appendToFile()` once per
  snapshot round, same cadence discipline as `AuditLog` already uses.

## Definition of done

- `cluster_snapshot.hpp` + its test build and pass (already true).
- Exactly one of `NativeController` or `DistributedRunner` calls `record()`
  per tracked host on a defined cadence and writes a snapshot line.
- A CLI-visible way to inspect the latest snapshot.
- Full test suite still green.
