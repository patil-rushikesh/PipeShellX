# Phase 1 — Lamport Logical Clocks

**Status: implemented.** This file is the original plan; where it differs
from the code, the code and `07-security-review.md` win (one shared clock
per node process, and the clock saturates at `2^64-1` instead of wrapping).

## Goal

Give every stage-dispatch event a Lamport timestamp so the controller and
nodes have a documented, code-backed happens-before ordering across the
cluster, instead of relying on wall-clock time (which is exactly the
textbook motivation for Lamport clocks: independent clocks on independent
machines aren't trustworthy for ordering).

## What already exists (done, don't redo)

`include/psx/runtime/lamport_clock.hpp` -- a small header-only class:

```cpp
namespace psx::runtime {
class LamportClock {
public:
    std::uint64_t tick();                 // local event: counter += 1, returns new value
    std::uint64_t observe(std::uint64_t received); // counter = max(counter, received) + 1
    std::uint64_t value() const noexcept; // current value, no mutation
private:
    std::uint64_t counter_ = 0;
};
}
```

No mutex: every reactor (`psx::runtime::Reactor`) in this codebase runs
single-threaded per its own doc comments ("Not thread-safe" appears on
`NativeController`, `NodeServer`, `DistributedRunner`, `NodeStageRunner`).
A `LamportClock` instance lives on the same reactor thread as the object
that owns it, so a plain `std::uint64_t` is correct and matches the
project's existing concurrency model. **Do not add a mutex or atomics here**
-- that would be inconsistent with the rest of the transport layer and is
a sign you've misread the ownership model.

`tests/unit/runtime/test_lamport_clock.cpp` -- covers: starts at 0, `tick()`
increments by 1 and returns the new value, `observe()` takes the max of
local/received then adds 1 (both when received > local and when received
<= local), and a short "two clocks passing messages" simulation that checks
the classic Lamport property (if event A happens-before event B via a
message, `timestamp(A) < timestamp(B)`).

## What's NOT done yet -- your task

### Task 1: bump `OpenRequest` to wire version 2

This is the sanctioned extension point (see `00-overview.md`). Below is the
exact code to use. It is backward compatible: a version-1 payload still
decodes as version 1 with `lamportTs = 0` (treat 0 as "not set" -- version-1
peers never had a clock, that's fine and expected).

**`include/psx/transport/open_request.hpp`** -- add a field and keep both
encode functions available during migration:

```cpp
struct OpenRequest {
    std::vector<std::string> argv;
    std::string cwd;
    std::uint64_t lamportTs = 0; // 0 = not set / peer is version 1

    bool operator==(const OpenRequest& other) const {
        return argv == other.argv && cwd == other.cwd && lamportTs == other.lamportTs;
    }
};

// Existing v1 encode, unchanged -- kept for any caller that still wants to
// send a v1 payload explicitly (e.g. a compatibility test).
std::string encodeOpen(const OpenRequest& request);

// New: v2 payload, adds the lamportTs field after cwd.
std::string encodeOpenV2(const OpenRequest& request);

psx::Result<OpenRequest> decodeOpen(std::string_view payload); // accepts v1 or v2
```

**`src/transport/open_request.cpp`** -- the new pieces:

```cpp
namespace {
constexpr std::uint8_t kOpenVersion1 = 1;
constexpr std::uint8_t kOpenVersion2 = 2;

// u64 helper -- wire.hpp only has u32; add these two functions there instead
// of duplicating them here (see Task 1a below).
} // namespace

std::string encodeOpenV2(const OpenRequest& request) {
    std::string out;
    out.push_back(static_cast<char>(kOpenVersion2));
    writeU32BE(out, static_cast<std::uint32_t>(request.argv.size()));
    for (const auto& arg : request.argv) {
        writeU32BE(out, static_cast<std::uint32_t>(arg.size()));
        out.append(arg);
    }
    writeU32BE(out, static_cast<std::uint32_t>(request.cwd.size()));
    out.append(request.cwd);
    writeU64BE(out, request.lamportTs);
    return out;
}

psx::Result<OpenRequest> decodeOpen(std::string_view payload) {
    Reader reader(payload);
    std::uint8_t version = 0;
    if (!reader.takeU8(version)) {
        return malformed("OPEN payload: missing version");
    }
    if (version != kOpenVersion1 && version != kOpenVersion2) {
        return malformed("OPEN payload: unsupported version");
    }
    std::uint32_t argc = 0;
    if (!reader.takeU32(argc)) {
        return malformed("OPEN payload: missing argc");
    }
    if (argc > kMaxOpenArgc) {
        return malformed("OPEN payload: argc exceeds the maximum");
    }
    OpenRequest request;
    request.argv.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
        std::string arg;
        if (!reader.takeString(arg, kMaxOpenArgumentBytes)) {
            return malformed("OPEN payload: truncated argument");
        }
        request.argv.push_back(std::move(arg));
    }
    if (!reader.takeString(request.cwd, kMaxOpenCwdBytes)) {
        return malformed("OPEN payload: truncated cwd");
    }
    if (version == kOpenVersion2) {
        if (!reader.takeU64(request.lamportTs)) {
            return malformed("OPEN payload: truncated lamport timestamp");
        }
    }
    if (reader.remaining() != 0) {
        return malformed("OPEN payload: trailing bytes");
    }
    if (auto valid = validateOpenRequest(request); !valid.ok()) {
        return valid.error();
    }
    return request;
}
```

`encodeOpen` (v1) stays exactly as it is today -- untouched, still emits
version 1, still omits `lamportTs`. This means old tests that build a v1
payload and decode it keep passing unchanged.

### Task 1a: add `u64` wire helpers

`include/psx/transport/wire.hpp` only has `writeU32BE`/`readU32BE`. Add the
64-bit equivalents there (same style, big-endian, inline):

```cpp
inline void writeU64BE(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}
inline std::uint64_t readU64BE(const char* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(p[i]);
    }
    return value;
}
```

Then add a `Reader::takeU64` in `open_request.cpp`'s anonymous-namespace
`Reader` class, mirroring `takeU32` exactly but reading 8 bytes.

### Task 2: check and update the existing test file

**Read `tests/unit/transport/test_open_request.cpp` before writing any new
test.** It almost certainly has a case asserting that a payload with
version byte `2` (or any version != 1) is rejected as "unsupported
version" -- that assertion is now wrong and must be changed to version `3`
(or whatever's still actually unsupported) so the test suite reflects the
new reality instead of failing on correct code. Do not delete the "reject
unknown version" test -- just change which version number it uses to test
rejection.

Add new cases: encode with `encodeOpenV2`, decode, check `lamportTs`
round-trips; decode a v1 payload (from `encodeOpen`), check `lamportTs ==
0`; a truncated v2 payload (missing the trailing 8 bytes) is rejected.

### Task 3: wire the clock into the controller and node

This is the part that touches files not fully read in this planning
session (`src/transport/native_controller.cpp`, `src/transport/
node_stage_runner.cpp`). Do this:

1. **Controller side** (wherever `NativeController`/`DistributedRunner`
   currently builds an `OpenRequest` and calls `encodeOpen` to send an
   `OPEN` frame): give the controller object a `psx::runtime::LamportClock
   member`, call `.tick()` right before building the `OpenRequest`, put the
   result in `lamportTs`, and switch that call site from `encodeOpen` to
   `encodeOpenV2`.
2. **Node side** (`NodeStageRunner::onOpen`, which receives a decoded
   `OpenRequest`): `NodeServer` owns ONE `LamportClock` and passes it by
   reference to every `NodeStageRunner` (a runner exists per connection, and
   a Lamport clock belongs to the process, not the connection -- a clock per
   runner was a bug, fixed as DS-02 in `07-security-review.md`). Call
   `.observe(request.lamportTs)` as the first thing `onOpen` does (skip the
   observe -- just `tick()` instead -- if `lamportTs == 0`, since that means
   a v1 peer with no clock).
3. **Log it**: extend `LogContext` (`include/logger.hpp`) with an optional
   `lamportTs` field (or just log it as part of the message string if you'd
   rather not touch `LogContext`'s shape -- simplest is fine here) so the
   ordering is visible in the log file, which is the actual deliverable for
   a report/demo: show two stages on two different nodes, dispatched
   concurrently, and their Lamport timestamps proving a consistent partial
   order even though wall-clock timestamps on the two machines may disagree.

## What NOT to do

- Do not touch `FrameType`, `Frame`, `PING`/`PONG`, or `EXIT` payload
  formats. `EXIT` is fixed at 5 bytes and multiple things assert that
  (`decodeExit` rejects anything != 5 bytes; `wire_protocol.md` documents
  it as fixed). Extending it would need the same kind of version-byte
  treatment `OPEN` already has, and `EXIT` doesn't have one today -- that's
  a bigger, riskier change than this phase needs.
- Do not make `encodeOpen` (v1) emit `lamportTs`. Its whole point is to stay
  the stable v1 encoder.
- Do not use wall-clock time anywhere in this feature. The entire point of
  a Lamport clock is that it's a counter, not a timestamp -- if you find
  yourself reaching for `std::chrono`, you've built something else.
- Do not add locking. One reactor thread per process; a bare
  `std::uint64_t` is correct.
- Do not try to make this a *vector* clock. That's a different (larger)
  data structure and isn't what's being asked for in this phase. If a
  future phase wants per-node causal history instead of a single total
  order, that's a separate task -- flag it, don't silently swap it in.

## Definition of done

- `lamport_clock.hpp` + its test build and pass (already true).
- `OpenRequest` v2 encode/decode built, tested, `test_open_request.cpp`
  updated and passing.
- `wire.hpp` has `writeU64BE`/`readU64BE`, tested (add a couple of cases to
  `tests/unit/transport/test_frame_codec.cpp` or wherever `wire.hpp` is
  currently exercised -- check first).
- Controller sends v2 `OPEN` with a real ticking `lamportTs`; node observes
  it and ticks its own clock; both show up in the log.
- Full test suite (`ctest` / `pipeshellx_tests`) still green.
