#pragma once

#include <cstdint>
#include <string>

namespace psx::transport {

// Big-endian (network order) 32-bit helpers shared by the frame envelope and the
// per-type payload codecs.
inline void writeU32BE(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

// Reads a big-endian u32 from `p`; the caller guarantees at least 4 readable bytes.
inline std::uint32_t readU32BE(const char* p) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
}

// Big-endian 64-bit helpers, same convention as the u32 pair above. Used by
// wire fields that need a wider range than u32 (e.g. a Lamport timestamp in
// OPEN v2 -- see docs/ds-project/01-lamport-clocks.md).
inline void writeU64BE(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

// Reads a big-endian u64 from `p`; the caller guarantees at least 8 readable bytes.
inline std::uint64_t readU64BE(const char* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(p[i]);
    }
    return value;
}

} // namespace psx::transport
