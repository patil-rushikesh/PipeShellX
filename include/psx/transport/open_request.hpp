#pragma once

#include "psx/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psx::transport {

// psx/1 bounds the version-1 process request independently of the enclosing
// frame limit. These limits keep decoding and eventual process creation
// predictable across supported platforms.
inline constexpr std::uint32_t kMaxOpenArgc = 1024;
inline constexpr std::uint32_t kMaxOpenArgumentBytes = 128U * 1024U;
inline constexpr std::uint32_t kMaxOpenCwdBytes = 32U * 1024U;

// The payload of an OPEN frame: what the controller asks a node to start.
// Kept deliberately small and versioned; env/limits/timeout can extend the wire
// format under a new version byte without breaking older readers.
struct OpenRequest {
    std::vector<std::string> argv; // argv[0] is the program to exec
    std::string cwd;               // empty = inherit the agent's working directory
    std::uint64_t lamportTs = 0;   // OPEN v2 only; 0 = not set / peer sent v1

    bool operator==(const OpenRequest& other) const {
        return argv == other.argv && cwd == other.cwd && lamportTs == other.lamportTs;
    }
};

// Validates the executable-facing invariants and psx/1 resource bounds. In
// particular, argv[0] must name a program and no string may contain NUL because
// the platform exec APIs would otherwise silently truncate it.
psx::Result<void> validateOpenRequest(const OpenRequest& request);

// Wire (big-endian), version 1:
//   u8  version (= 1)
//   u32 argc
//   argc × [ u32 len, len bytes ]   (each argument)
//   u32 cwdLen, cwdLen bytes
// encodeOpen always emits version 1 and never includes lamportTs; it exists
// so a v1 payload can still be produced explicitly (e.g. compatibility tests).
std::string encodeOpen(const OpenRequest& request);

// Wire (big-endian), version 2 -- adds a trailing Lamport timestamp field
// after cwd. See docs/ds-project/01-lamport-clocks.md.
//   u8  version (= 2)
//   u32 argc
//   argc × [ u32 len, len bytes ]
//   u32 cwdLen, cwdLen bytes
//   u64 lamportTs
std::string encodeOpenV2(const OpenRequest& request);

// Decodes an OPEN payload of either version. Returns an error (protocol
// violation) on an unknown version, invalid request, truncated field, or an
// out-of-bounds length — never reads out of bounds or loops on an
// attacker-controlled unbounded argc. A version-1 payload decodes with
// lamportTs == 0.
psx::Result<OpenRequest> decodeOpen(std::string_view payload);

} // namespace psx::transport
