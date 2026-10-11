# DS Course Add-On — Overview

**Purpose of this folder:** PipeShellX is being extended for a Distributed
Systems course project/FA. This folder is the working plan. It exists so
that work can be picked up by a different person or a different LLM session
without re-deriving context. Read this file first, then the numbered file
for whatever phase you're working on.

## Framing (read this before writing any code)

PipeShellX is **already a distributed system**: a controller process and N
worker (node) processes, separate address spaces, separate failure domains,
coordinating over a network (mTLS, custom `psx/1` wire protocol). This is
the Master-Worker / Coordinator-Worker architecture — the same category as
Hadoop MapReduce (JobTracker/TaskTracker), Kubernetes (control-plane/kubelet),
Slurm. It is not "not distributed" just because control is centralized.

What it currently has **no code for** (see `docs/architecture.md` L18, which
explicitly says so): logical clocks, election, distributed mutual exclusion,
global snapshots, distributed deadlock detection. That's the real gap versus
a typical DS syllabus, and closing part of it is the point of this folder.

The wire protocol (`psx/1`, see `docs/wire_protocol.md`) is **frozen and
conformance-tested**: the doc explicitly states "New frame types require
session-layer capability negotiation" and existing frame formats are fixed
byte layouts asserted by `tests/unit/transport/*`. Do not casually change
`FrameType`, `Frame`, `PING`/`PONG` payload shape, or the 5-byte `EXIT`
payload. The one sanctioned extension point is `OpenRequest`
(`include/psx/transport/open_request.hpp`), whose own doc comment says:
"Kept deliberately small and versioned; env/limits/timeout can extend the
wire format under a new version byte without breaking older readers."
Use that door, not a new one.

## Phases

| Phase | File | What it adds | Unit | Status |
| --- | --- | --- | --- | --- |
| 1 | `01-lamport-clocks.md` | Lamport clocks on stage dispatch (OPEN v2), one shared clock per node, saturation | III | Done, tested |
| 2 | `02-cluster-snapshot.md` | Global-state snapshots (simplified Chandy-Lamport), strict parser, `snapshot dump` | III | Done, tested |
| 3 | `03-election-stretch.md` | Bully election with epochs and leader beacon | III | Done (state machine + simulator; no socket driver yet) |
| 4 | `06-ds-concepts-detailed.md` section 4.6 | Ricart-Agrawala mutual exclusion with replay protection | III | Done (state machine + simulator) |
| 5 | `06-ds-concepts-detailed.md` section 5.6 | Hash-chained, verifiable audit log (ledger) | IV | Done, wired into `run --audit-log` |
| — | `pipeshellx sim` | Demo simulator: reordering, crashes, restarts, invariant checks | III | Done |
| Docs | `05-testing-guide.md` | How to build, run and demo everything (commands verified) | — | Done |
| Docs | `06-ds-concepts-detailed.md` | Syllabus mapping, Units I-IV, with honest limits | all | Done |
| Docs | `07-security-review.md` | Code-by-code review: findings, fixes, residual risks | — | Done |
| Docs | `08-presentation-flow.md` | FA-2 slide-by-slide flow with live demo script | — | Done |

Verification: 609 tests pass (107 DS-specific); new code is clean under
ASan/UBSan and fuzzed; every safety check is mutation-tested. Details are in
`07-security-review.md`.

## Cross-cutting rules

See `04-dos-and-donts.md` for the full list. The short version: match
existing style exactly (`psx::` namespaces, `psx::Result<T>` for fallible
calls, no exceptions for control flow, single-reactor-thread => no mutexes
in new runtime code), add new source files to the relevant `CMakeLists.txt`,
write a GoogleTest file for anything new under `tests/unit/`, and never
weaken an existing protocol invariant to make a feature easier to add.

## What is where

| Concept | Header / source | Tests | CLI |
| --- | --- | --- | --- |
| Lamport clock | `include/psx/runtime/lamport_clock.hpp` | `test_lamport_clock.cpp`, integration in `test_native_transport.cpp` | `node status` (`lamport_clock`) |
| OPEN v2 | `include/psx/transport/open_request.hpp`, `docs/wire_protocol.md` | `test_open_request.cpp`, `test_wire.cpp` | — |
| Global state | `include/psx/runtime/cluster_snapshot.hpp` | `test_cluster_snapshot.cpp` | `run --snapshot-file`, `snapshot dump` |
| Strict JSON reader | `include/psx/json/reader.hpp` | `test_json_reader.cpp` | — |
| Ricart-Agrawala | `include/psx/sync/ricart_agrawala.hpp` | `test_ricart_agrawala.cpp`, `test_simulation.cpp` | `sim mutex` |
| Bully election | `include/psx/election/bully.hpp` | `test_bully.cpp`, `test_simulation.cpp` | `sim election` |
| Simulator | `include/psx/sim/simulation.hpp` | `test_simulation.cpp`, `test_sim_command.cpp` | `sim` |
| Hash-chained audit | `include/psx/audit/audit_log.hpp`, `sha256.hpp` | `test_audit_chain.cpp` | `run --audit-log`, `audit verify` |
