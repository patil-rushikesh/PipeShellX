# Security and correctness review: distributed-systems code

Scope: every file added or changed for the DS course work on the `red`
branch: Lamport clocks, OPEN v2, cluster snapshots, the snapshot CLI, and the
new Ricart-Agrawala, Bully election, simulator, and hash-chained audit log.
Each finding below was fixed, has a regression test, and (where it guards a
safety property) was mutation-tested: the check was deliberately broken to
confirm a test fails.

## How it was verified

| Check | Result |
| --- | --- |
| Full test suite (excluding SSH suites that need an `ssh` binary) | 609 passed (baseline 542; +67 new) |
| SSH-dependent suites vs. untouched `red` | identical failure sets (environmental, not regressions) |
| AddressSanitizer + UndefinedBehaviorSanitizer, new/changed suites | 171 passed, no reports, no leaks |
| Fuzzing: JSON reader, snapshot parser, OPEN v2, mutex and election decoders | 400,000 mutated inputs under ASan/UBSan, no faults |
| Mutation testing of every safety check | every mutant caught (table at the end) |
| End-to-end: real CA, node daemon, two controllers over mTLS, snapshot, audit verify, tamper | all behaved as specified |

## Findings in the existing DS code (all fixed)

| ID | Severity | Where | Problem | Fix | Test |
| --- | --- | --- | --- | --- | --- |
| DS-01 | Medium | `runtime/lamport_clock.hpp` | `observe(UINT64_MAX)` computed `max+1`, wrapping to **0**. OPEN v2 carries this value off the wire, so one message from any authenticated controller (or a bug) reset a node's clock, making later events look older than earlier ones. | Saturate at `2^64-1`; `saturated()` lets callers refuse to order further events (Ricart-Agrawala does). | `LamportClockTest.ObserveOfMaxValueSaturatesInsteadOfWrapping` and 2 more |
| DS-02 | Medium | `transport/node_server.*`, `node_stage_runner.*` | The node's Lamport clock lived in `NodeStageRunner`, which exists **per controller connection**. One node process had several unrelated clocks, so events from two controllers were never ordered. That contradicts the definition (one clock per process) and the docs. | `NodeServer` owns one clock and passes it to every runner. It is declared before `connections_` so it outlives them. Exposed as `lamport_clock` in `node status`. | `LamportClockIntegrationTest.NodeClockIsSharedAcrossControllerConnections` (node clock = 3, was 2); live run |
| DS-03 | Medium | `runtime/cluster_snapshot.cpp` | The JSONL "parser" was substring search: `]` or `}` inside a host string truncated the node list; a value ending in `\\` ran the field past its end; `\uXXXX` became the literal `uXXXX`; a negative `lamport_ts` cast to `18446744073709551615`; duplicate keys were resolved arbitrarily. | Replaced with a strict JSON reader (`json/reader.*`): proper strings and escapes, integer range checks, depth and size limits, duplicate keys rejected. | `JsonReaderTest.*` (7), `ClusterSnapshotTest.HostileStringsRoundTripExactly`, `RejectsNegativeLamportTimestamp...`, `RejectsDuplicateFieldsAndWrongTypes` |
| DS-04 | Medium | `ClusterSnapshot::readFromFile` | `std::getline` buffered an unbounded line, so one multi-GB line in a shared or corrupted file exhausted memory in `snapshot dump`. | Byte-wise read with a 4 MiB per-line cap; over-long lines are skipped without being stored. | `ReadFromFileSkipsOverlongLineWithoutBufferingIt` |
| DS-05 | Low | `ClusterSnapshot::formatTable` | Host, status and run id were printed raw to the terminal. Once `\u001b` decodes correctly, a crafted snapshot file can inject ANSI escape sequences (rewrite the screen, set the title, hide text) into the operator's terminal. | Control characters are replaced with `?` in table output. | `FormatTableNeutralisesTerminalEscapes` |
| DS-06 | Medium | `NativeController::takeSnapshot` | Every launch and completion rewrote every node synchronously on the reactor thread: O(N²) bytes, about 100 MB for 1,000 hosts. Worse, blocking the reactor delays PING/PONG, so a large fleet could trip its **own** lease timeout and fence healthy nodes. | Coalesced: at most one write per 250 ms, plus a guaranteed final snapshot at completion. The timer is cancelled in the destructor because its callback captures `this`. | `ClusterSnapshotIntegrationTest.ControllerRecordsNodeSnapshotsToDisk` (final state still written) |
| DS-07 | Low | `NativeController::captureSnapshot` | Never-dispatched targets were stamped with the controller's *current* clock, a logical timestamp for an event that never happened. | Report `0` for undispatched targets. | `UndispatchedTargetHasNoLamportTimestamp` |
| DS-08 | Low | snapshot and audit files | Created with the process umask (often world-readable). Audit records contain the commands run, which can include secrets passed as arguments. | Newly created files are `0600`; existing files keep their owner's chosen mode. | `NewSnapshotFileIsOwnerOnly`, `NewLogIsOwnerOnly`; live `-rw-------` |
| DS-09 | Medium (claim) | `audit/audit_log.cpp`, doc 06 | Docs called the audit log "tamper-evident" and "immutable". It was a plain file anyone with write access could edit silently. | SHA-256 hash chain (`prev`/`hash` on every record), `pipeshellx audit verify`, `--expect-head` anchoring. Limits are documented (see R-05). | `AuditChainTest.*` (8), `Sha256Test.*` incl. OpenSSL cross-check |
| DS-10 | Low | `AuditLog` reopen | A crash mid-write left a partial last line, and the next record would be glued onto it. | On reopen, a missing trailing newline is terminated first; verify reports the partial record as a break. | `PartialLastRecordIsTerminatedAndReported` |
| DS-11 | Low (privacy) | `.gitignore` | `fleet.ini` (real user, host and LAN IP) was not ignored; `git add .` would publish it. | Ignored. | — |

## Hardening designed into the new code

| ID | Component | Threat | Defence |
| --- | --- | --- | --- |
| N-01 | Ricart-Agrawala | A replayed or delayed REPLY from an earlier round counts toward the current one and lets a node in while another holds the section. Textbook RA is vulnerable to this. | Every REPLY echoes the request timestamp it answers; a mismatch is rejected. |
| N-02 | Ricart-Agrawala | Duplicate REPLY, unsolicited REPLY, non-member sender, wrong recipient, a second pending request | Rejected as protocol errors **before** any state changes, including the Lamport clock. |
| N-03 | Bully | A delayed COORDINATOR or BEACON from a crashed leader makes nodes follow a dead node. | Election epochs (like Raft terms); older-epoch messages are ignored. |
| N-04 | Bully | Messages that cannot occur in a correct run (ELECTION from a higher id, ANSWER from a lower id) | Rejected. |
| N-05 | Bully | A wrapped epoch would make every legitimate message look stale; `leaderTimeout <= beaconInterval` would re-elect forever. | Epoch saturates; `create()` rejects flapping timeouts. |
| N-06 | Wire codecs | Truncation, trailing bytes, unknown version or type | Exact-length decoding with version and type checks; fuzzed. |
| N-07 | `pipeshellx sim` | Hostile arguments | `from_chars` with the whole token required, explicit ranges, at most 64 nodes and 10 minutes of simulated time. |

## Residual risks (accepted, documented, not fixed)

| ID | Risk | Why it stays | What would fix it |
| --- | --- | --- | --- |
| R-01 | OPEN v2 has no capability negotiation; a pre-v2 node rejects it and drops the connection. | Course scope; `wire_protocol.md` states it. | Negotiate capabilities in the handshake, or upgrade nodes before controllers. |
| R-02 | Ricart-Agrawala and Bully trust the `from` field. They currently run in the simulator, not over sockets. | No network driver exists yet. | A driver must take `from` from the mTLS SAN of the connection, never from the payload. A forged high id would otherwise win every Bully election. |
| R-03 | RA with `removePeer` is only as safe as the failure detector: a partitioned but alive node could still be inside the section. | Inherent to RA. | Quorum-based mutual exclusion (Maekawa) or leases with fencing tokens. |
| R-04 | Bully is not partition-tolerant (split brain: one leader per side). | Inherent to Bully. | A majority quorum (Raft). The epoch can serve as a fencing token for anything a leader writes. |
| R-05 | A hash chain alone does not stop a writer who rewrites the chain from an edit onward, or truncates it. | Hash chains only make edits *evident* relative to a trusted head. | Keep the head elsewhere (`--expect-head`); for stronger guarantees, sign the head with the controller key. |
| R-06 | Two processes appending to one audit log fork the chain. | Detected by verify, not prevented. | An advisory file lock, which is itself a mutual-exclusion problem. |
| R-07 | Snapshots record local state only (no in-flight channel state), so they are not a Chandy-Lamport consistent cut. | In a star topology where every channel ends at the controller this is close, but not identical. | Marker-based snapshot with channel recording. |
| R-08 | Lamport timestamps flow only controller → node; `EXIT` (fixed 5 bytes) carries none back. | Changing `EXIT` needs a versioned payload. | `EXIT` v2 with the node's clock. |

## Mutation testing of the safety checks

| Mutant (deliberate bug) | Caught by |
| --- | --- |
| RA: holder replies instead of deferring | 3 RA tests + simulator property test (seed 4: "two nodes inside together") |
| RA: inverted (ts, id) priority | tie-break test, randomized fairness property test |
| RA: no echoed-timestamp check (textbook) | `ReplayedReplyFromEarlierRoundIsRejected` |
| Bully: no epoch check on COORDINATOR | `StaleCoordinatorFromAnOlderEpochIsIgnored` |
| Bully: no take-over on ELECTION from lower id | `LowestNodeElectionIsTakenOverByTheBully` |
| Bully: no beacon-silence failure detection | 4 tests |
| Audit verify: skip content hash | `ModifiedRecordIsDetected`, CLI test |
| Audit verify: skip prev link | `DeletedInsertedAndReorderedRecordsAreDetected` |
