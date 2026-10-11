#include "psx/json/reader.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using psx::json::parse;

TEST(JsonReaderTest, ParsesObjectsArraysStringsIntsBoolsNull) {
    auto v = parse(R"({"a":1,"b":[true,false,null],"c":"x","d":-42})");
    ASSERT_TRUE(v.ok()) << v.error().message();
    ASSERT_NE(v.value().find("a"), nullptr);
    EXPECT_EQ(*v.value().find("a")->asInt(), 1);
    EXPECT_EQ(*v.value().find("d")->asInt(), -42);
    EXPECT_EQ(*v.value().find("c")->asString(), "x");
    const auto* arr = v.value().find("b")->asArray();
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->size(), 3U);
    EXPECT_TRUE(*(*arr)[0].asBool());
    EXPECT_TRUE((*arr)[2].isNull());
}

// The old substring scanner cut the node list at the first ']' even inside a
// string, and mis-handled a value ending in an escaped backslash.
TEST(JsonReaderTest, BracketsAndEscapedBackslashInsideStringsAreData) {
    auto v = parse(R"({"host":"a]}b","path":"c:\\","next":"ok"})");
    ASSERT_TRUE(v.ok()) << v.error().message();
    EXPECT_EQ(*v.value().find("host")->asString(), "a]}b");
    EXPECT_EQ(*v.value().find("path")->asString(), "c:\\");
    EXPECT_EQ(*v.value().find("next")->asString(), "ok");
}

TEST(JsonReaderTest, DecodesUnicodeEscapesIncludingSurrogatePairs) {
    auto v = parse(R"(["\u001b","\u00e9","\ud83d\ude00"])");
    ASSERT_TRUE(v.ok()) << v.error().message();
    const auto& a = *v.value().asArray();
    EXPECT_EQ(*a[0].asString(), std::string("\x1b"));
    EXPECT_EQ(*a[1].asString(), "\xC3\xA9");
    EXPECT_EQ(*a[2].asString(), "\xF0\x9F\x98\x80");
}

TEST(JsonReaderTest, RejectsMalformedInput) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "\"unterminated", "[1 2]", "tru", "01", "1.5",
                            "1e3", "{\"a\":1}x", "\"\\q\"", "\"\\ud800\"", "\"\\udc00\"", "[\"a\nb\"]"}) {
        EXPECT_FALSE(parse(bad).ok()) << "accepted: " << bad;
    }
}

// Two readers that disagree on which duplicate wins are a classic smuggling
// vector; this reader refuses the ambiguity outright.
TEST(JsonReaderTest, RejectsDuplicateKeys) {
    EXPECT_FALSE(parse(R"({"status":"exited","status":"lost"})").ok());
}

TEST(JsonReaderTest, IntegerBoundsAreExact) {
    EXPECT_TRUE(parse("9223372036854775807").ok());
    EXPECT_TRUE(parse("-9223372036854775808").ok());
    EXPECT_EQ(*parse("-9223372036854775808").value().asInt(), std::numeric_limits<std::int64_t>::min());
    EXPECT_FALSE(parse("9223372036854775808").ok());
    EXPECT_FALSE(parse("-9223372036854775809").ok());
    EXPECT_FALSE(parse("99999999999999999999999").ok());
}

TEST(JsonReaderTest, DepthAndSizeAreBounded) {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    EXPECT_FALSE(parse(deep).ok()) << "unbounded recursion on hostile nesting";
    EXPECT_FALSE(parse(std::string(64, ' ') + "1", {.maxBytes = 16, .maxDepth = 32}).ok());
}
