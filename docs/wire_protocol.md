# psx/1 Native Wire Protocol

## Status and scope

This document specifies the protocol implemented by the PipeShellX native
controller and node. The protocol name is **psx/1**. It multiplexes command
stages over a mutually authenticated TLS connection and carries stage input,
stdout, stderr, exit status, flow-control credit, and connection liveness.

The current implementation does not support reconnect/resume. A broken or
expired connection is terminal: the controller reports the affected work as
lost and the node fences the stages owned by that connection.

## Transport and identity

- The byte stream is TLS 1.3, implemented with OpenSSL 3.x.
- Both peers present certificates chaining to the configured fleet CA.
- Peer identity is the certificate's SAN URI. After certificate validation,
  each endpoint applies its configured SAN allow-list/pin.
- An optional CA-issued CRL is checked during the handshake.
- No application frame is accepted before TLS authentication and authorization
  complete.

The protocol has no clear-text negotiation preface. Controllers and nodes must
be configured for compatible psx/1 implementations before connecting.

## Frame envelope

Every frame has a fixed 10-byte header followed by `length` payload bytes. All
multi-byte integers are unsigned big-endian unless stated otherwise.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | `type` | Frame type |
| 1 | 1 | `flags` | Type-specific bit flags |
| 2 | 4 | `stream_id` | Logical stage stream; zero for connection frames |
| 6 | 4 | `length` | Payload byte count |
| 10 | `length` | `payload` | Type-specific bytes |

The default maximum payload is 16 MiB. A larger advertised length is a fatal
protocol error, and a decoder remains poisoned after that error. Receivers may
deliver TLS bytes in arbitrary chunks; a frame is dispatched only once its
entire header and payload have arrived.

Unknown frame types decode at the envelope layer for forward compatibility but
are rejected by the psx/1 session layer.

## Frame types

| Value | Name | Direction | Stream | Payload |
| ---: | --- | --- | --- | --- |
| 1 | `OPEN` | controller to node | nonzero, new | Versioned stage request |
| 2 | `DATA` | either direction | open | Raw stdin/stdout/stderr bytes |
| 3 | `WINDOW_UPDATE` | either direction | open | Four-byte credit delta |
| 4 | `EXIT` | node to controller | open | Five-byte exit status |
| 5 | `PING` | either direction | zero | Empty |
| 6 | `PONG` | either direction | zero | Empty |
| 7 | `GOAWAY` | either direction | zero | Empty |

Flag bit `0x01` is `END_STREAM`. On `DATA`, flag bit `0x02` selects stderr;
without it, node-to-controller data is stdout. Controller-to-node `DATA` is
stage stdin and must not set `STDERR`. The accepted flags are deliberately
narrow: `OPEN` and `WINDOW_UPDATE` require flags `0`; `EXIT` accepts either
flags `0` or `END_STREAM`; and `DATA` accepts flags `0` or any combination of
`END_STREAM` and `STDERR`. `PING`, `PONG`, and `GOAWAY` are connection frames and
therefore require stream ID `0`, flags `0`, and an empty payload. Reserved flag
bits are fatal protocol errors.

## OPEN payload version 1

An `OPEN` frame starts one stage. Only a controller may open a stream. Stream
identifiers are gapless sequential integers starting at `1`; they cannot be
skipped or reused on a connection. An endpoint that has either sent or
received `GOAWAY` cannot open or accept another stream.

```
u8   version = 1
u32  argc
repeat argc times:
  u32  argument_length
  byte argument[argument_length]
u32  cwd_length
byte cwd[cwd_length]
```

`argv[0]` is the executable. An empty working directory inherits the node
agent's working directory. Version 1 permits at most 1,024 arguments, at most
128 KiB per argument, and at most 32 KiB for `cwd`. The argv vector and
`argv[0]` must be nonempty. No argument or `cwd` may contain an embedded NUL.
Unknown versions, fields beyond those bounds, truncated fields, and trailing
bytes are protocol errors. Environment, limits, timeout, PTY, and resume
metadata are not part of version 1.

## OPEN payload version 2 (Lamport timestamp)

Version 2 is version 1 followed by one field: the controller's Lamport clock
value at the moment it dispatched the stage.

```
u8   version = 2
... identical to version 1 (argc, arguments, cwd) ...
u64  lamport_ts          # big-endian
```

The node applies the Lamport receive rule, `clock = max(clock, lamport_ts) + 1`,
to a single clock shared by every controller connection of that node process.
A `lamport_ts` of `0` means "not set". The clock saturates at `2^64 - 1`
instead of wrapping, so a peer cannot reset it by sending the maximum value.

A controller sends version 2 only when it has a timestamp to carry; otherwise
it sends the exact version-1 bytes. Nodes accept both versions. **Compatibility
limit:** there is no capability negotiation, so a node built before version 2
rejects a version-2 `OPEN` as an unsupported version and the connection fails.
Upgrade nodes before controllers in a mixed fleet.

## DATA and half-close

`DATA` preserves frame order within a connection. Bytes are opaque; psx/1 does
not impose text encoding or line boundaries.

`END_STREAM` half-closes the sender's data direction after that frame. An empty
`DATA|END_STREAM` is valid and is how the controller sends immediate EOF to a
stage that has no stdin. A peer must not send new data after half-close. stdout
and stderr have distinct channel flags but share a stream lifecycle and credit
window.

## Flow control

Each stream starts with 256 KiB of receive credit in each direction. A sender
must not transmit more payload bytes than its remaining credit. The receiver
returns credit only after the application consumes bytes:

```
WINDOW_UPDATE payload: u32 delta
```

A `WINDOW_UPDATE` has flags `0`, a nonzero open stream ID, and exactly the
four-byte payload above. A zero delta, credit overflow beyond `0x7fffffff`, or
data beyond the advertised window is a fatal protocol error. When credit is
exhausted, the sender queues at most its configured high-water amount and must
stop its producer until a `WINDOW_UPDATE` makes the stream writable again.
End-of-stream and `EXIT` frames queued behind data are emitted only after
preceding bytes have drained.

## EXIT payload

The node terminates a stream with exactly one `EXIT` frame:

```
u8   kind       # 0 exited, 1 signaled, 2 terminated
i32  code       # big-endian two's-complement bits
```

`EXIT` is ordered after all stdout/stderr data for that stage and removes the
stream at both peers. Its only accepted flag values are `0` and `END_STREAM`.
A duplicate exit, an exit sent by a controller, or an exit for an unknown
stream is a protocol error. Application-level nonzero exit codes are valid
stage outcomes, not transport errors.

## Liveness and connection drain

Either peer may send `PING`; the receiver immediately answers `PONG`. Any valid
inbound frame proves peer activity. Production transports ping every two
seconds and fail after three consecutive silent intervals, surfacing a silent
partition in approximately six seconds.

`GOAWAY` announces graceful connection drain. Sending or receiving it blocks
new stream opens in both roles; existing streams may finish.

## Error handling and fencing

Malformed payloads, invalid role/direction, unknown or closed stream use,
flow-control violations, TLS/authentication failure, SAN authorization failure,
and lease expiry are fatal to the connection. A semantic frame error poisons
the `Session`: all later receives fail and it emits no automatic response.
Implementations close the transport rather than trying to resynchronize.

There is one tightly scoped close-race exception. An otherwise-valid `DATA` or
`WINDOW_UPDATE` that crosses an `EXIT` sent locally may be ignored because the
two frames traveled in opposite directions. Traffic that cannot be such a
crossing remains fatal: same-direction traffic after an `EXIT` received from
the peer is rejected, and any `DATA` after an `END_STREAM` received from that
peer is rejected.

On connection loss, a node terminates and reaps all stages owned by the lost
controller connection. A controller marks unfinished stages as failed/lost; it
must not invent successful exit codes. Reconnect-and-resume and byte sequence
acknowledgements are reserved for a later protocol version.

## Compatibility rules

- The 10-byte envelope is stable for psx/1.
- Structured payloads carry their own version byte where evolution is expected.
- New frame types require session-layer capability negotiation before they can
  be sent to psx/1 peers; envelope-level decoding alone is not negotiation.
- Existing flag bits must retain their meaning. Unknown flag bits are rejected
  unless a future negotiated extension defines them.

The executable code in `include/psx/transport/` and the protocol tests in
`tests/unit/transport/` are the conformance reference for this document.
