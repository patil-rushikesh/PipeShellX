#include "psx/json/reader.hpp"

#include <limits>

namespace psx::json {

const Value* Value::find(std::string_view key) const noexcept {
    const Object* object = asObject();
    if (object == nullptr) {
        return nullptr;
    }
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
}

namespace {

psx::Error fail(const char* what) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, what};
}

class Parser {
public:
    Parser(std::string_view text, Limits limits) : text_(text), limits_(limits) {}

    psx::Result<Value> document() {
        if (text_.size() > limits_.maxBytes) {
            return fail("json: document exceeds size limit");
        }
        skipSpace();
        auto value = parseValue(0);
        if (!value.ok()) {
            return value;
        }
        skipSpace();
        if (pos_ != text_.size()) {
            return fail("json: trailing characters after value");
        }
        return value;
    }

private:
    psx::Result<Value> parseValue(std::size_t depth) {
        if (depth > limits_.maxDepth) {
            return fail("json: nesting too deep");
        }
        if (pos_ >= text_.size()) {
            return fail("json: unexpected end of input");
        }
        switch (text_[pos_]) {
            case '{':
                return parseObject(depth);
            case '[':
                return parseArray(depth);
            case '"': {
                auto s = parseString();
                if (!s.ok()) {
                    return s.error();
                }
                return Value(Value::Storage(std::move(s.value())));
            }
            case 't':
                return literal("true", Value(Value::Storage(true)));
            case 'f':
                return literal("false", Value(Value::Storage(false)));
            case 'n':
                return literal("null", Value());
            default:
                return parseInteger();
        }
    }

    psx::Result<Value> literal(std::string_view word, Value value) {
        if (text_.substr(pos_, word.size()) != word) {
            return fail("json: invalid literal");
        }
        pos_ += word.size();
        return value;
    }

    psx::Result<Value> parseInteger() {
        bool negative = false;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            negative = true;
            ++pos_;
        }
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
            return fail("json: invalid value");
        }
        if (text_[pos_] == '0' && pos_ + 1 < text_.size() && text_[pos_ + 1] >= '0' && text_[pos_ + 1] <= '9') {
            return fail("json: leading zero in number");
        }
        // Accumulate as a negative magnitude so INT64_MIN is representable.
        std::int64_t value = 0;
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            const int digit = text_[pos_] - '0';
            if (value < (kMin + digit) / 10) {
                return fail("json: integer out of range");
            }
            value = value * 10 - digit;
            ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            return fail("json: only integer numbers are supported");
        }
        if (!negative) {
            if (value == kMin) {
                return fail("json: integer out of range");
            }
            value = -value;
        }
        return Value(Value::Storage(value));
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    bool readHex4(std::uint32_t& out) {
        if (text_.size() - pos_ < 4) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexValue(text_[pos_ + static_cast<std::size_t>(i)]);
            if (h < 0) {
                return false;
            }
            out = (out << 4U) | static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    psx::Result<std::string> parseString() {
        ++pos_; // opening quote
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                return fail("json: unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("json: raw control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("json: unterminated escape");
            }
            const char e = text_[pos_++];
            switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!readHex4(cp)) {
                        return fail("json: invalid \\u escape");
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (text_.substr(pos_, 2) != "\\u") {
                            return fail("json: unpaired surrogate");
                        }
                        pos_ += 2;
                        if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return fail("json: unpaired surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10U) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("json: unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail("json: invalid escape");
            }
        }
    }

    psx::Result<Value> parseArray(std::size_t depth) {
        ++pos_; // '['
        Array items;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return Value(Value::Storage(std::move(items)));
        }
        while (true) {
            skipSpace();
            auto item = parseValue(depth + 1);
            if (!item.ok()) {
                return item;
            }
            items.push_back(std::move(item.value()));
            skipSpace();
            if (pos_ >= text_.size()) {
                return fail("json: unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return Value(Value::Storage(std::move(items)));
            }
            return fail("json: expected ',' or ']'");
        }
    }

    psx::Result<Value> parseObject(std::size_t depth) {
        ++pos_; // '{'
        Object members;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return Value(Value::Storage(std::move(members)));
        }
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("json: expected object key");
            }
            auto key = parseString();
            if (!key.ok()) {
                return key.error();
            }
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("json: expected ':'");
            }
            ++pos_;
            skipSpace();
            auto value = parseValue(depth + 1);
            if (!value.ok()) {
                return value;
            }
            if (!members.emplace(std::move(key.value()), std::move(value.value())).second) {
                return fail("json: duplicate object key");
            }
            skipSpace();
            if (pos_ >= text_.size()) {
                return fail("json: unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return Value(Value::Storage(std::move(members)));
            }
            return fail("json: expected ',' or '}'");
        }
    }

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    std::string_view text_;
    Limits limits_;
    std::size_t pos_ = 0;
};

} // namespace

psx::Result<Value> parse(std::string_view text, Limits limits) {
    return Parser(text, limits).document();
}

} // namespace psx::json
