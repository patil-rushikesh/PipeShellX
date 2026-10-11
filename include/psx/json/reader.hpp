#pragma once

// psx::json::parse — a small, strict JSON reader for PipeShellX's own JSONL
// records (cluster snapshots today). It replaces ad-hoc substring scanning,
// which mis-parsed strings containing ']' / '}' / an escaped backslash and
// silently dropped \uXXXX escapes.
//
// Scope, deliberately narrow:
//   * objects, arrays, strings, booleans, null, and integers that fit int64;
//   * fractional/exponent numbers are rejected (no PipeShellX record has one);
//   * \uXXXX escapes are decoded to UTF-8; unpaired surrogates are rejected;
//   * nesting depth and input size are bounded (hostile input cannot recurse
//     the stack or allocate without limit);
//   * duplicate object keys are rejected, so "status":"ok","status":"lost"
//     cannot mean one thing to this reader and another to a different one.
// Anything outside that is a parse error rather than a guess.

#include "psx/result.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace psx::json {

class Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

class Value {
public:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;

    Value() : v_(nullptr) {}
    explicit Value(Storage v) : v_(std::move(v)) {}

    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(v_); }
    const bool* asBool() const noexcept { return std::get_if<bool>(&v_); }
    const std::int64_t* asInt() const noexcept { return std::get_if<std::int64_t>(&v_); }
    const std::string* asString() const noexcept { return std::get_if<std::string>(&v_); }
    const Array* asArray() const noexcept { return std::get_if<Array>(&v_); }
    const Object* asObject() const noexcept { return std::get_if<Object>(&v_); }

    // Member lookup on an object; nullptr when not an object or key absent.
    const Value* find(std::string_view key) const noexcept;

private:
    Storage v_;
};

struct Limits {
    std::size_t maxBytes = 4U * 1024U * 1024U; // whole document
    std::size_t maxDepth = 32;                 // nested arrays/objects
};

// Parses exactly one JSON value; leading/trailing whitespace is allowed,
// anything else after the value is an error.
psx::Result<Value> parse(std::string_view text, Limits limits = {});

} // namespace psx::json
