#include "psx/transport/wire.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace psx::transport;

TEST(WireTest, U32RoundTrips) {
    std::string out;
    writeU32BE(out, 0x01020304u);
    ASSERT_EQ(out.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(out[0]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(out[3]), 0x04);
    EXPECT_EQ(readU32BE(out.data()), 0x01020304u);
}

TEST(WireTest, U64RoundTrips) {
    std::string out;
    writeU64BE(out, 0x0102030405060708ULL);
    ASSERT_EQ(out.size(), 8U);
    EXPECT_EQ(static_cast<unsigned char>(out[0]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(out[7]), 0x08);
    EXPECT_EQ(readU64BE(out.data()), 0x0102030405060708ULL);
}

TEST(WireTest, U64HandlesZeroAndMax) {
    std::string zero;
    writeU64BE(zero, 0);
    EXPECT_EQ(readU64BE(zero.data()), 0ULL);

    std::string max;
    writeU64BE(max, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(readU64BE(max.data()), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(WireTest, U64WritesInBigEndianOrder) {
    std::string out;
    writeU64BE(out, 1); // least significant byte only
    ASSERT_EQ(out.size(), 8U);
    for (int i = 0; i < 7; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(out[i]), 0x00) << "byte " << i;
    }
    EXPECT_EQ(static_cast<unsigned char>(out[7]), 0x01);
}
