#pragma once

// SHA-256 (FIPS 180-4), self-contained so the audit hash chain works in
// builds without OpenSSL (PIPESHELLX_NATIVE_TRANSPORT=OFF). Verified against
// the NIST test vectors and, when OpenSSL is present, against EVP in tests.
// Not constant-time-relevant: it hashes public log records, not secrets.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace psx::audit {

class Sha256 {
public:
    Sha256();
    void update(std::string_view data);
    std::array<std::uint8_t, 32> finish(); // the object must not be reused after finish()

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t bufLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

// Lower-case hex SHA-256 of `data` (64 characters).
std::string sha256Hex(std::string_view data);

} // namespace psx::audit
