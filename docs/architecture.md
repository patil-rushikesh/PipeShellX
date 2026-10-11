# Architecture

PipeShellX 0.6 is a C++20 command-execution controller for Linux and macOS
and, when native transport is enabled, a node daemon. One lowercase
`pipeshellx` executable provides `run`, `ping`, `diff`,
`pipe`, `hosts`, `ca`, `node`, and the legacy
`shell` REPL.

This document is the contributor-facing architecture contract. User-visible
pipeline behavior is specified in [Pipelines](pipelines.md), the native wire
contract in [psx/1](wire_protocol.md), deployment boundaries in
[Deployment](deployment.md), and unfinished work in the
[Roadmap](../ROADMAP.md).

## Architectural boundaries

- A run has one coordinator: the CLI process that started it. The production
  controller is not replicated and PipeShellX has no database or consensus
  protocol. `--consensus` and `diff` compare host output; they are reduction
  operations, not agreement protocols. Bully leader election and
  Ricart-Agrawala mutual exclusion exist as tested, transport-agnostic state
  machines (`include/psx/election/`, `include/psx/sync/`) exercised by
  `pipeshellx sim`; they are not yet wired between real controller processes
  (see `docs/ds-project/`).
- SSH is the agentless transport. The native psx/1 node is optional and exists
  for multiplexed, channel-aware execution and remote pipeline edges.
- The operator and each node's operating-system account are trust boundaries.
  Authentication grants command-execution authority unless an optional
  controller or node policy narrows it. PipeShellX is not a multi-tenant
  sandbox; see [Security policy](../SECURITY.md).
- Public `include/psx/**` headers expose standard C++ types. Production
  platform headers and system calls belong to `src/os/**`; tests and the
  benchmark harness may use platform APIs to verify the abstraction.
- Windows is an SSH-target tier only. There is no Win32 controller, native
  node, IOCP backend, Job Object integration, SCM service, or Windows CI job.

## Layers and dependency direction

Dependencies point downward. `scripts/check_layering.sh` enforces the public
type, platform-header, private-backend, and upward-dependency rules.

| Layer | Responsibilities | Principal locations |
| --- | --- | --- |
| L5 interfaces | Command dispatch, strict option parsing, product exit codes | `src/main.cpp`, `src/cli/` |
| L4 product/orchestration | Inventory and selection, policy, audit, sinks, fan-out, pipeline planning and runners | `src/{inventory,policy,audit,sink,pipeline}/`, `src/process_manager.cpp` |
| L3 transports | Managed OpenSSH workers; psx/1 session, TLS stream, controller and node | `src/ssh_transport.cpp`, `src/transport/` |
| L2 streams | Retained buffers, spooling, line framing, credit and stream state | `src/stream/` |
| L1 runtime | Single-threaded I/O, child, signal, and timer dispatch | `src/runtime/reactor.cpp` |
| L0 OS | Handles, pipes, processes, polling, child/signal sources, sockets, TLS, paths and console | `include/psx/os/`, `src/os/` |

Fallible lower-layer operations return `psx::Result<T>` with a portable
`psx::Error` and raw platform error code. Expected failures such as
`WouldBlock`, `BrokenPipe`, and `Closed` are values, not exceptions.
Configuration and usage code in the upper layers may throw to a command
boundary that translates the error to exit `2`.

## Key decisions and rationale

### System OpenSSH, not an embedded SSH stack

Agentless execution starts `ssh` from `PATH` as a managed local
process. Reusing the system client preserves the operator's OpenSSH config,
agents, certificates, hardware keys, proxy/jump rules, and security-update
path. Embedding another SSH implementation would duplicate a
security-sensitive stack and lose those integrations; requiring the native
node everywhere would remove the zero-install mode.

OpenSSH owns authentication, host-key verification, and the target login
shell. PipeShellX uses a per-inventory trust store and can opt into
ControlMaster reuse. The controller does not insert a local shell, but SSH
serializes the command for the target shell, so SSH execution is not
shell-free.

### One coordinator, no control-plane consensus

Availability comes from explicit retry of eligible SSH work and from native
connection leases/fencing, not controller replication. Loss of a native
controller connection terminates stages owned by that connection; unfinished
controller results fail rather than acquire invented successful exits. There
is no reconnect/resume or duplicate-spawn ledger in psx/1.

### POSIX readiness today; completion tension remains for Windows

The current `Reactor` exposes readiness callbacks over nonblocking handles.
Linux `epoll`, Darwin/BSD `kqueue`, and the portable
`poll` backend all fit that contract: a callback drains until
`WouldBlock` and pauses interest when a downstream buffer is full.

IOCP is completion-based rather than readiness-based, and overlapped Windows
pipes cannot be modeled faithfully with the same kernel operations. A future
Win32 port must either adapt submitted completions behind the tested reactor
contract or supersede the contract explicitly. The current code does not
pretend that an IOCP backend exists.

### Fixed TLV envelope, structured payloads

psx/1 uses a fixed 10-byte, big-endian frame header containing type, flags,
stream ID, and payload length. A fixed envelope is trivial to delimit from a
chunked TLS byte stream, has a small bounded decoder surface, and avoids a
CBOR dependency and self-describing framing overhead. Structured payloads,
such as `OPEN`, carry their own version and bounds. Exact field and
state-machine rules are normative in [the wire protocol](wire_protocol.md).

### OpenSSL 3 through memory BIOs

Native transport uses mutual TLS 1.3 with caller-supplied CA, optional CRL,
and SAN-URI identity checks. OpenSSL 3 provides the same CA/CRL/SAN behavior on
supported POSIX platforms. `psx::os::Tls` owns memory BIOs rather than a
socket: the reactor moves ciphertext through `psx::os::Socket`, while
the TLS engine transforms bytes independently. This keeps OpenSSL types out of
public headers and keeps socket ownership explicit. Static OpenSSL selection
is a build preference, not an architectural guarantee of a fully static
binary.

## OS and runtime mapping

| Concern | Linux | macOS / BSD | Portable fallback | Windows status |
| --- | --- | --- | --- | --- |
| Owned handle | `int`; close-on-exec creation/duplication | `int`; `FD_CLOEXEC` set before the handle is returned | same POSIX contract | not implemented |
| Pipe | `pipe2(O_CLOEXEC)` | `pipe` then immediate `fcntl(FD_CLOEXEC)`; this is not an atomic creation primitive | POSIX pipe | overlapped pipe not implemented |
| Process | `posix_spawn`; post-spawn `prlimit` where requested | `posix_spawn`; narrow async-signal-safe fork fallback for limits | POSIX spawn path | `CreateProcessW`/Job Objects not implemented |
| Readiness | edge-triggered `epoll` + `eventfd` wake | `kqueue` with `EV_CLEAR` + user-event wake | level-triggered `poll` + self-pipe | IOCP not implemented |
| Child exit | `pidfd`, with signal-driven fallback | `EVFILT_PROC` | `SIGCHLD`, self-pipe, `waitid(WNOWAIT)` | job completion port not implemented |
| Signals | `signalfd` | `EVFILT_SIGNAL` | signal/self-pipe source | console control source not implemented |
| Network/control | nonblocking TCP and AF_UNIX sockets | nonblocking TCP and AF_UNIX sockets | POSIX sockets | Winsock/named control pipe not implemented |
| TLS | OpenSSL 3 memory BIO | OpenSSL 3 memory BIO | native transport requires OpenSSL 3 | native node not implemented |

The `PIPESHELLX_POLLER=poll|epoll|kqueue` environment variable can
force a supported backend. The portable backend is a behavior oracle, not a
different product mode.

## Principal flows

```text
run
  parse -> optional controller policy -> inventory + selector
      -> explicit/per-host transport decision
      -> bounded SSH worker window OR native controller
      -> channel-aware sink -> summary + optional audit

ping
  parse -> inventory + selector -> SSH probe window -> ONLINE/OFFLINE

diff
  parse -> inventory + selector -> native mTLS fan-out
      -> successful exit-0 stdout buckets -> unanimous/drift/failure

pipe
  parse inline/file -> validate identifiers, edges, cycle and placement
      -> all-local declared DAG runner
      OR require a linear remote graph -> segmented local/native runner

node
  load TLS identity/authorization/optional policy -> accept connection
      -> psx/1 session -> validate OPEN -> process group + channel streams
      -> EXIT or connection-loss fencing -> reap
```

`run` uses one transport for the selected host set. An explicit
`--transport ssh|native` overrides inventory metadata; otherwise a
mixed selection is rejected. `ping` is SSH-only. `diff` and
remote `pipe` are native-only.

## Lifecycle and resource invariants

### Descriptor ownership

- Every kernel handle has one RAII owner and is closed exactly once.
- Linux creates pipes close-on-exec atomically. Platforms without
  `pipe2` set `FD_CLOEXEC` immediately and before returning the
  handle; contributors must not describe that two-call sequence as atomic.
- A child receives only descriptors 0, 1, and 2 plus an explicitly granted
  extra descriptor such as the `sshpass -d` password pipe.
- After spawn, the parent owns only its stdin writer and stdout/stderr readers.
  EOF or teardown unregisters and closes them.

### Process lifetime

- Each child is placed in its own process group. Timeout, failure propagation,
  cancellation, and owner teardown signal the group rather than only its
  leader.
- The child-exit source reports a specific child without reaping it. The
  owning process object performs exactly one reap and caches the status;
  `waitpid(-1)` is not used.
- A missing executable is a synchronous spawn failure surfaced as exit
  `127`. A signal-terminated legacy process-manager result uses exit
  `-1`.
- After a forced stop, the controller drains remaining output for a bounded
  two-second grace. A descriptor holder outside the owned process group cannot
  stall completion indefinitely.

### I/O, EOF, and backpressure

- Readiness handlers drain nonblocking I/O until `WouldBlock`. stdin
  writers close after their final byte so the stage observes EOF.
- stdout and stderr remain distinct through SSH/native capture, psx/1 flags,
  sinks, `diff`, and audit outcome metadata.
- Every local DAG edge has a bounded buffer. A full successor edge pauses the
  producer; fan-out is governed by the slowest live successor, and a closed
  successor removes only its edge.
- Native streams have bounded credit. Credit limits bytes in flight, not the
  amount a lossless controller sink may ultimately retain.
- Run capture is deliberately policy-dependent: drop policies bound retained
  bytes and report loss; `block` capture is lossless and unbounded;
  spool bounds its memory tail but not temporary-disk growth or final
  materialization. These are product limits, not violations hidden by the
  architecture.

### Failure and fencing

- A stage is successful only after a real zero exit with no timeout,
  cancellation, abort, or transport failure.
- Fail-fast prevents pending work and stops in-flight siblings after a final
  failure. Ctrl-C cancels owned work and maps to product exit `130`.
- A native connection owns its node stages. TLS/protocol/lease failure closes
  the connection and fences those stages. psx/1 semantic errors poison the
  session rather than attempting resynchronization.
- A downstream pipeline exit fences unfinished upstream work and waits for
  cancellation/reap accounting; pipefail uses the rightmost real failure in
  deterministic planner order.

## Current limitations

- Linux and macOS are the only controller and native-node platforms. Windows
  can be reached only as an OpenSSH target from a POSIX controller.
- General fan-in/fan-out DAG execution is local. Any graph containing a remote
  stage must be one declared chain; SSH does not carry pipeline edges.
- Native reconnect/resume, byte acknowledgements, and duplicate-spawn
  suppression are not implemented.
- Hard-killing the node itself is not a universal descendant-containment
  guarantee; node-death containment remains platform work.
- Per-stage configurable limits, privilege separation, sandboxing, locked
  secret memory, and signed/tamper-evident audit are not implemented.
- Lossless capture and spool completion are not memory/disk bounded, and
  psx/1 has no connection-wide credit window.
- Publishing the first signed release, heterogeneous-fleet qualification,
  continuous fuzzing, and the Windows matrix remain roadmap work; the signed
  Linux/macOS release workflow itself is implemented.

See [ROADMAP.md](../ROADMAP.md) for only the unfinished commitments; completed
history belongs in version control and the changelog rather than the roadmap.
