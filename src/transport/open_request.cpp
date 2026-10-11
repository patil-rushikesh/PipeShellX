#include "psx/transport/open_request.hpp"

#include "psx/transport/wire.hpp"

namespace psx::transport {

namespace {

constexpr std::uint8_t kOpenVersion1 = 1;
constexpr std::uint8_t kOpenVersion2 = 2;

psx::Error malformed(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}

// A bounds-checked forward cursor over the payload.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool takeU8(std::uint8_t& out) {
        if (remaining() < 1) {
            return false;
        }
        out = static_cast<std::uint8_t>(data_[pos_++]);
        return true;
    }
    bool takeU32(std::uint32_t& out) {
        if (remaining() < 4) {
            return false;
        }
        out = readU32BE(data_.data() + pos_);
        pos_ += 4;
        return true;
    }
    bool takeU64(std::uint64_t& out) {
        if (remaining() < 8) {
            return false;
        }
        out = readU64BE(data_.data() + pos_);
        pos_ += 8;
        return true;
    }
    // Reads a bounded u32 length then that many bytes.
    bool takeString(std::string& out, std::uint32_t maxLength) {
        std::uint32_t len = 0;
        if (!takeU32(len) || len > maxLength || remaining() < len) {
            return false;
        }
        out.assign(data_.data() + pos_, len);
        pos_ += len;
        return true;
    }
    std::size_t remaining() const { return data_.size() - pos_; }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

} // namespace

psx::Result<void> validateOpenRequest(const OpenRequest& request) {
    if (request.argv.empty()) {
        return malformed("OPEN payload: argv must not be empty");
    }
    if (request.argv.size() > kMaxOpenArgc) {
        return malformed("OPEN payload: argc exceeds the maximum");
    }
    if (request.argv.front().empty()) {
        return malformed("OPEN payload: argv[0] must not be empty");
    }
    for (const std::string& argument : request.argv) {
        if (argument.size() > kMaxOpenArgumentBytes) {
            return malformed("OPEN payload: argument exceeds the maximum");
        }
        if (argument.find('\0') != std::string::npos) {
            return malformed("OPEN payload: argument contains NUL");
        }
    }
    if (request.cwd.size() > kMaxOpenCwdBytes) {
        return malformed("OPEN payload: cwd exceeds the maximum");
    }
    if (request.cwd.find('\0') != std::string::npos) {
        return malformed("OPEN payload: cwd contains NUL");
    }
    return {};
}

std::string encodeOpen(const OpenRequest& request) {
    std::string out;
    out.push_back(static_cast<char>(kOpenVersion1));
    writeU32BE(out, static_cast<std::uint32_t>(request.argv.size()));
    for (const auto& arg : request.argv) {
        writeU32BE(out, static_cast<std::uint32_t>(arg.size()));
        out.append(arg);
    }
    writeU32BE(out, static_cast<std::uint32_t>(request.cwd.size()));
    out.append(request.cwd);
    return out;
}

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

} // namespace psx::transport
