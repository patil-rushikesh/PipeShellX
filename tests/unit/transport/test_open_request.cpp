#include "psx/transport/open_request.hpp"

#include "psx/transport/wire.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using psx::transport::decodeOpen;
using psx::transport::encodeOpen;
using psx::transport::kMaxOpenArgc;
using psx::transport::kMaxOpenArgumentBytes;
using psx::transport::kMaxOpenCwdBytes;
using psx::transport::OpenRequest;

TEST(OpenRequestTest, RoundTripsArgvAndCwd) {
    const OpenRequest in{.argv = {"/bin/sh", "-c", "echo hi"}, .cwd = "/tmp/work"};
    const auto out = decodeOpen(encodeOpen(in));
    ASSERT_TRUE(out.ok()) << out.error().message();
    EXPECT_EQ(out.value(), in);
}

TEST(OpenRequestTest, HandlesEmptyCwdAndOpaqueNonNulArguments) {
    for (const OpenRequest& in : {
             OpenRequest{.argv = {"x"}, .cwd = ""},
             OpenRequest{.argv = {"x", "", "c"}, .cwd = "d\ne"},
         }) {
        const auto out = decodeOpen(encodeOpen(in));
        ASSERT_TRUE(out.ok()) << out.error().message();
        EXPECT_EQ(out.value(), in);
    }
}

TEST(OpenRequestTest, RejectsEmptyArgvAndEmptyArgvZero) {
    for (const OpenRequest& in : {
             OpenRequest{.argv = {}, .cwd = ""},
             OpenRequest{.argv = {""}, .cwd = ""},
             OpenRequest{.argv = {"", "argument"}, .cwd = "/tmp"},
         }) {
        EXPECT_FALSE(decodeOpen(encodeOpen(in)).ok());
    }
}

TEST(OpenRequestTest, RejectsEmbeddedNulInEveryExecVisibleString) {
    for (const OpenRequest& in : {
             OpenRequest{.argv = {std::string("a\0b", 3)}, .cwd = ""},
             OpenRequest{.argv = {"x", std::string("a\0b", 3)}, .cwd = ""},
             OpenRequest{.argv = {"x"}, .cwd = std::string("a\0b", 3)},
         }) {
        EXPECT_FALSE(decodeOpen(encodeOpen(in)).ok());
    }
}

TEST(OpenRequestTest, RejectsV1FieldsAboveTheirResourceBounds) {
    OpenRequest tooMany{.argv = std::vector<std::string>(kMaxOpenArgc + 1, "x"), .cwd = ""};
    EXPECT_FALSE(decodeOpen(encodeOpen(tooMany)).ok());

    OpenRequest longArgument{.argv = {"x", std::string(kMaxOpenArgumentBytes + 1, 'a')}, .cwd = ""};
    EXPECT_FALSE(decodeOpen(encodeOpen(longArgument)).ok());

    OpenRequest longCwd{.argv = {"x"}, .cwd = std::string(kMaxOpenCwdBytes + 1, 'c')};
    EXPECT_FALSE(decodeOpen(encodeOpen(longCwd)).ok());
}

TEST(OpenRequestTest, AcceptsV1FieldsAtTheirResourceBounds) {
    OpenRequest atLimits{.argv = std::vector<std::string>(kMaxOpenArgc, ""), .cwd = std::string(kMaxOpenCwdBytes, 'c')};
    atLimits.argv.front() = std::string(kMaxOpenArgumentBytes, 'a');
    const auto decoded = decodeOpen(encodeOpen(atLimits));
    ASSERT_TRUE(decoded.ok()) << decoded.error().message();
    EXPECT_EQ(decoded.value(), atLimits);
}

TEST(OpenRequestTest, VersionByteIsFirst) {
    const std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    EXPECT_EQ(static_cast<unsigned char>(wire[0]), 1u);
}

TEST(OpenRequestTest, RejectsAnUnknownVersion) {
    std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    wire[0] = 3; // version 2 is now valid (OPEN v2, adds lamportTs); 3 is not
    EXPECT_FALSE(decodeOpen(wire).ok());
}

TEST(OpenRequestTest, V2RoundTripsArgvCwdAndLamportTimestamp) {
    const OpenRequest in{.argv = {"/bin/sh", "-c", "echo hi"}, .cwd = "/tmp/work", .lamportTs = 42};
    const auto out = decodeOpen(encodeOpenV2(in));
    ASSERT_TRUE(out.ok()) << out.error().message();
    EXPECT_EQ(out.value(), in);
    EXPECT_EQ(out.value().lamportTs, 42U);
}

TEST(OpenRequestTest, V1PayloadDecodesWithZeroLamportTimestamp) {
    const OpenRequest in{.argv = {"x"}, .cwd = ""};
    const auto out = decodeOpen(encodeOpen(in)); // v1 encoder
    ASSERT_TRUE(out.ok()) << out.error().message();
    EXPECT_EQ(out.value().lamportTs, 0U);
}

TEST(OpenRequestTest, V2VersionByteIsTwo) {
    const std::string wire = encodeOpenV2(OpenRequest{.argv = {"x"}, .cwd = ""});
    EXPECT_EQ(static_cast<unsigned char>(wire[0]), 2u);
}

TEST(OpenRequestTest, V2RejectsTruncatedLamportTimestamp) {
    std::string wire = encodeOpenV2(OpenRequest{.argv = {"x"}, .cwd = "", .lamportTs = 0x0102030405060708ULL});
    // Drop bytes one at a time off the end of the 8-byte lamportTs field only.
    for (int drop = 1; drop <= 8; ++drop) {
        const std::string truncated = wire.substr(0, wire.size() - drop);
        EXPECT_FALSE(decodeOpen(truncated).ok()) << "dropped " << drop << " byte(s)";
    }
    EXPECT_TRUE(decodeOpen(wire).ok()); // the full v2 payload still decodes
}

TEST(OpenRequestTest, RejectsTruncatedInput) {
    const std::string wire = encodeOpen(OpenRequest{.argv = {"hello"}, .cwd = "/w"});
    // Every proper prefix that stops mid-field must be rejected, never over-read.
    for (std::size_t n = 0; n < wire.size(); ++n) {
        EXPECT_FALSE(decodeOpen(wire.substr(0, n)).ok()) << "prefix length " << n;
    }
    EXPECT_TRUE(decodeOpen(wire).ok()); // the full thing decodes
}

TEST(OpenRequestTest, RejectsTrailingBytes) {
    std::string wire = encodeOpen(OpenRequest{.argv = {"x"}, .cwd = ""});
    wire.push_back('!');
    EXPECT_FALSE(decodeOpen(wire).ok());
}

TEST(OpenRequestTest, RejectsAGarbageArgcWithoutHugeAllocationOrOverread) {
    // version=1, argc=0xFFFFFFFF, then no argument bytes → must fail on the first
    // truncated argument, not loop 4 billion times or over-read.
    std::string wire;
    wire.push_back(1);
    psx::transport::writeU32BE(wire, 0xFFFFFFFFu);
    EXPECT_FALSE(decodeOpen(wire).ok());
}
