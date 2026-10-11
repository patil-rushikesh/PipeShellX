# Phase 3: Bully election for controller HA

**Status: implemented** as a transport-agnostic state machine
(`include/psx/election/bully.hpp`, `src/election/bully.cpp`), with epochs and a
leader beacon. It is tested in `tests/unit/election/test_bully.cpp` and by
property tests in `tests/unit/sim/test_simulation.cpp`, and demonstrated with
`pipeshellx sim election`. Not built yet: a socket driver that runs real
controller processes as candidates. That driver must take the sender id from
the mTLS SAN (see `07-security-review.md`, R-02). The design notes below are
the original plan, kept for the report.

## Goal

Today there is exactly one controller; if it dies mid-run, the run is lost
(the protocol doc says this outright: "a controller marks unfinished
stages as failed/lost; it must not invent successful exit codes" -- there's
no resume). Bully election demonstrates the syllabus's "Election
Algorithms" topic by letting multiple controller *candidates* agree on
which one is currently active, without needing full consensus (Raft/Paxos)
machinery.

## Shape of the design

- A small, fixed set of controller candidate processes, each configured
  with a numeric ID (higher = higher priority, standard Bully rule) and the
  addresses of the others.
- A **separate** control connection between candidates (not the existing
  `psx/1` controller<->node backplane -- do not try to route election
  traffic over the node-facing protocol, that protocol is scoped to
  controller-to-node stage execution and mixing concerns there is exactly
  the kind of invasive change `00-overview.md` says to avoid). A second,
  much simpler protocol is fine: candidates only ever exchange three
  message kinds (`ELECTION`, `OK`, `COORDINATOR`), classic Bully.
- Only the currently-elected leader is allowed to open `NativeController`/
  `NodeServer` connections to the real worker fleet. Non-leader candidates
  stay idle, watching for the leader's heartbeat.
- On leader heartbeat silence (reuse the same "3 missed pings -> declare
  dead" rule the existing `psx/1` liveness logic already uses, for
  consistency), a candidate starts an election: sends `ELECTION` to every
  higher-ID candidate, waits briefly for `OK`; if none arrives, it declares
  itself leader and broadcasts `COORDINATOR`; if an `OK` arrives, it steps
  back and waits for a `COORDINATOR` message.
- On becoming leader, the new controller needs cluster state to resume
  from -- this is where Phase 2's `ClusterSnapshot` actually earns its
  keep as a real recovery mechanism rather than just an observability
  artifact: the new leader reads the last snapshot file to know what was
  running, then decides (out of scope to actually resume execution -- that
  needs the wire protocol's reserved "reconnect/resume" feature, which
  `wire_protocol.md` explicitly says is not implemented -- for this project
  it's enough to *report* what was lost, matching the existing "no invented
  successful exit codes" rule).

## Suggested file layout (not created yet)

- `include/psx/election/bully_node.hpp` / `src/election/bully_node.cpp` --
  new top-level module, new namespace `psx::election`, doesn't depend on
  `psx::transport` beyond reusing `psx::runtime::Reactor` for its own
  sockets.
- `include/psx/election/messages.hpp` -- the 3-message wire format for
  election traffic; keep it deliberately tiny (a 1-byte message-type tag +
  a 4-byte candidate ID is enough, don't copy the full `psx/1` frame
  envelope for this, it's overkill for 3 message kinds).
- `tests/unit/election/test_bully_node.cpp` -- at minimum: highest-ID
  candidate always wins a simultaneous election; a mid-priority candidate
  that starts an election but sees a higher one respond backs off
  correctly; leader failure triggers a new election among survivors.

## What NOT to do

- Do not reuse `psx/1` frame types or extend `FrameType` for this. Election
  traffic is a different concern with different peers (controller
  candidates, not controller-to-node); keep it a separate, small protocol.
- Do not implement Raft or Paxos instead "because it's better." The
  syllabus item is Election Algorithms (Bully/Ring), and Bully is the
  correct, scoped answer -- consensus protocols are a different (much
  larger) topic and would blow the time budget for a course project.
- Do not attempt real stage-execution resume/reconnect on failover. The
  protocol doc reserves that for a future version; building it now means
  building a second, undocumented protocol extension on top of an
  already-ambitious stretch goal. Report losses honestly instead, same as
  the existing single-controller failure path already does.

## Definition of done (if you get here)

- Three (or more) controller-candidate processes can be started, exactly
  one self-elects as leader, and killing the leader causes a new election
  within a bounded time (document the bound you chose and why).
- The new leader can read the last `ClusterSnapshot` file and print what it
  believes was lost.
- A short write-up (for the report) explaining the Bully choice, the
  simplifications versus full Raft/Paxos, and what "resume" would need
  that wasn't built.
