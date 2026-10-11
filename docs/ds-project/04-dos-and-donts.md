# Cross-cutting Do's and Don'ts

Applies to every phase in this folder. If you (human or LLM) are about to
write code for this project and haven't read this file yet, read it first.

## Do

- Match existing style exactly: `psx::<layer>` namespaces (`psx::runtime`,
  `psx::transport`, `psx::election`, ...), `#pragma once`, doc comments
  above declarations explaining *why*, not just *what*.
- Use `psx::Result<T>` for anything that can fail (see `result.hpp` and how
  `decodeOpen`/`decodeExit` use it) -- never throw exceptions for expected
  failure modes (malformed wire data, bad config, etc). This codebase does
  not use exceptions for control flow anywhere you've seen so far; don't
  introduce the first instance.
- Build errors with `psx::Error{psx::ErrorClass::Other, 0, "message"}`
  matching the `malformed()` helper pattern already in
  `control_payloads.cpp` / `open_request.cpp`.
- Big-endian wire integers everywhere on the wire, using `wire.hpp`'s
  helpers (extend that file, don't duplicate the read/write logic locally).
- Add every new `.cpp` to the relevant `CMakeLists.txt`
  (`src/CMakeLists.txt` for library sources, `tests/CMakeLists.txt` for
  test sources) in the same commit that adds the file -- an unregistered
  `.cpp` silently never compiles or runs.
- Write a GoogleTest file under `tests/unit/<area>/` for every new module,
  following the naming (`test_<module>.cpp`) and structure of neighboring
  test files in that directory.
- Keep new runtime/transport objects single-threaded (no mutexes, no
  atomics) unless you have a specific, stated reason they're touched from
  more than one thread -- this matches every existing class in
  `psx::runtime` and `psx::transport`.
- Run the *actual* build and test suite before calling anything "done."
  This planning session did not compile anything -- it read source, it
  didn't execute `cmake`/`ctest`. Don't skip that step just because a
  previous session wrote code that looks right.
- Keep the DS-course framing honest in whatever gets submitted: say
  explicitly which syllabus items are fully implemented, which are
  simplified (and how), and which are deliberately out of scope, the same
  way this folder does. Graders respond better to accurate scoping than to
  overclaiming.

## Don't

- Don't modify `FrameType`, `Frame`, or any *existing* frame's payload
  shape (`PING`, `PONG`, `GOAWAY`, `WINDOW_UPDATE`, `DATA`, and especially
  `EXIT`'s fixed 5 bytes). All of these are asserted by
  `tests/unit/transport/test_frame_codec.cpp` and
  `tests/unit/transport/test_control_payloads.cpp`, and are documented as
  stable in `docs/wire_protocol.md`'s "Compatibility rules" section. If a
  feature seems to need this, it's a sign to redesign the feature (see
  Phase 1's choice to extend `OpenRequest` instead), not to touch the
  envelope.
- Don't add new frame types without also implementing the "session-layer
  capability negotiation" the protocol doc says is a prerequisite. None of
  the phases in this folder need a new frame type -- if a future idea
  seems to need one, that's a bigger design task than anything scoped
  here.
- Don't reach for `std::chrono`/wall-clock time as a stand-in for a
  logical clock. It defeats the entire pedagogical point.
- Don't guess at the internals of a `.cpp` file you haven't opened. Several
  integration points in Phase 1/2/3 explicitly call this out because this
  planning session read headers, not every implementation file -- verify
  against the real file before writing the integration code, don't pattern
  -match from the header alone.
- Don't fold Phase 3 into Phase 1/2's connections. Election traffic is a
  different protocol between different peers (controller candidates, not
  controller-to-node) -- keep it a separate module.
- Don't implement every syllabus item just to tick a box. `00-overview.md`
  already lists what's deliberately out of scope (P2P/WebRTC, distributed
  file systems, serverless, blockchain, full Raft/Paxos) -- forcing those
  in would look bolted-on. A short, honest "why we scoped this out"
  paragraph in the report is worth more than a fake integration.
