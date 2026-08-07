#include "support/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace livewall {
namespace {

const JsonValue kNull;
const std::string kEmpty;

// A single-pass recursive-descent parser over a string_view. `ok` latches
// false on the first syntax error and every level checks it, so a malformed
// document unwinds without exceptions.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skipSpace();
        JsonValue value = parseValue(0);
        if (!ok_) return {};
        skipSpace();
        // Trailing content means the document is not what it claims to be.
        if (pos_ != text_.size()) return {};
        return value;
    }

    bool ok() const { return ok_; }

private:
    // Bounds recursion so a hostile or corrupt file cannot blow the stack. Real
    // indexes are two levels deep.
    static constexpr int kMaxDepth = 32;

    std::string_view text_;
    size_t pos_ = 0;
    bool ok_ = true;

    void fail() { ok_ = false; }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    JsonValue parseValue(int depth) {
        if (!ok_ || depth > kMaxDepth) {
            fail();
            return {};
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            fail();
            return {};
        }

        switch (text_[pos_]) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                std::string s;
                if (!parseString(&s)) return {};
                return JsonValue(std::move(s));
            }
            case 't':
                if (literal("true")) return JsonValue(true);
                fail();
                return {};
            case 'f':
                if (literal("false")) return JsonValue(false);
                fail();
                return {};
            case 'n':
                if (literal("null")) return JsonValue();
                fail();
                return {};
            default: return parseNumber();
        }
    }

    JsonValue parseObject(int depth) {
        JsonValue result = JsonValue::object();
        ++pos_;  // '{'
        skipSpace();
        if (consume('}')) return result;

        for (;;) {
            skipSpace();
            std::string key;
            if (!parseString(&key)) return {};
            skipSpace();
            if (!consume(':')) {
                fail();
                return {};
            }
            JsonValue value = parseValue(depth + 1);
            if (!ok_) return {};
            result.set(std::move(key), std::move(value));

            skipSpace();
            if (consume(',')) continue;
            if (consume('}')) return result;
            fail();
            return {};
        }
    }

    JsonValue parseArray(int depth) {
        JsonValue result = JsonValue::array();
        ++pos_;  // '['
        skipSpace();
        if (consume(']')) return result;

        for (;;) {
            JsonValue value = parseValue(depth + 1);
            if (!ok_) return {};
            result.push(std::move(value));

            skipSpace();
            if (consume(',')) continue;
            if (consume(']')) return result;
            fail();
            return {};
        }
    }

    bool parseString(std::string* out) {
        if (!consume('"')) {
            fail();
            return false;
        }
        out->clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (pos_ >= text_.size()) break;
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    unsigned int code = 0;
                    if (!parseHex4(&code)) return false;
                    // Surrogate pair: the low half has to be read before the
                    // code point can be encoded, or the result is two
                    // unpaired surrogates that no UTF-8 decoder will accept.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned int low = 0;
                        if (!parseHex4(&low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }
                    appendUtf8(out, code);
                    break;
                }
                default:
                    fail();
                    return false;
            }
        }
        fail();
        return false;
    }

    bool parseHex4(unsigned int* out) {
        if (pos_ + 4 > text_.size()) {
            fail();
            return false;
        }
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail();
                return false;
            }
        }
        *out = value;
        return true;
    }

    static void appendUtf8(std::string* out, unsigned int code) {
        if (code < 0x80) {
            *out += static_cast<char>(code);
        } else if (code < 0x800) {
            *out += static_cast<char>(0xC0 | (code >> 6));
            *out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            *out += static_cast<char>(0xE0 | (code >> 12));
            *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (code >> 18));
            *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    JsonValue parseNumber() {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9')) {
                digits = true;
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        if (!digits) {
            fail();
            return {};
        }
        // strtod over a bounded copy: the view is not null-terminated, and the
        // numbers here are short enough that the copy is free.
        const std::string token(text_.substr(start, pos_ - start));
        return JsonValue(std::strtod(token.c_str(), nullptr));
    }
};

void escapeInto(std::string* out, std::string_view text) {
    *out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    *out += buffer;
                } else {
                    // Everything above 0x1F passes through, so UTF-8 stays
                    // UTF-8 rather than being expanded into \u escapes.
                    *out += c;
                }
        }
    }
    *out += '"';
}

std::string formatNumber(double value) {
    if (!std::isfinite(value)) return "0";
    // Whole numbers are the common case here — sizes, counts, frame rates — and
    // "1920" reads better in an index a user might open than "1920.0".
    if (value == std::floor(value) && std::fabs(value) < 9.0e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

}  // namespace

bool JsonValue::boolValue(bool fallback) const {
    return type_ == Type::Bool ? boolean_ : fallback;
}

double JsonValue::numberValue(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

std::int64_t JsonValue::intValue(std::int64_t fallback) const {
    if (type_ != Type::Number) return fallback;
    return static_cast<std::int64_t>(std::llround(number_));
}

const std::string& JsonValue::stringValue() const {
    return type_ == Type::String ? text_ : kEmpty;
}

bool JsonValue::has(std::string_view key) const {
    return type_ == Type::Object && members_.find(key) != members_.end();
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (type_ != Type::Object) return kNull;
    const auto it = members_.find(key);
    return it == members_.end() ? kNull : it->second;
}

void JsonValue::push(JsonValue value) {
    type_ = Type::Array;
    array_.push_back(std::move(value));
}

void JsonValue::set(std::string key, JsonValue value) {
    type_ = Type::Object;
    members_[std::move(key)] = std::move(value);
}

void JsonValue::erase(std::string_view key) {
    if (type_ != Type::Object) return;
    const auto it = members_.find(key);
    if (it != members_.end()) members_.erase(it);
}

std::string JsonValue::serialize(bool pretty, int indent) const {
    const std::string pad(pretty ? static_cast<size_t>(indent) * 2 : 0, ' ');
    const std::string padInner(pretty ? static_cast<size_t>(indent + 1) * 2 : 0, ' ');
    const char* newline = pretty ? "\n" : "";
    const char* space = pretty ? " " : "";

    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return boolean_ ? "true" : "false";
        case Type::Number: return formatNumber(number_);
        case Type::String: {
            std::string out;
            escapeInto(&out, text_);
            return out;
        }
        case Type::Array: {
            if (array_.empty()) return "[]";
            std::string out = "[";
            out += newline;
            for (size_t i = 0; i < array_.size(); ++i) {
                out += padInner;
                out += array_[i].serialize(pretty, indent + 1);
                if (i + 1 < array_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "]";
            return out;
        }
        case Type::Object: {
            if (members_.empty()) return "{}";
            std::string out = "{";
            out += newline;
            size_t index = 0;
            for (const auto& [key, value] : members_) {
                out += padInner;
                escapeInto(&out, key);
                out += ":";
                out += space;
                out += value.serialize(pretty, indent + 1);
                if (++index < members_.size()) out += ",";
                out += newline;
            }
            out += pad;
            out += "}";
            return out;
        }
    }
    return "null";
}

JsonValue parseJson(std::string_view text) {
    Parser parser(text);
    return parser.parse();
}

std::vector<JsonValue> parseArrayLenient(std::string_view text, int* dropped) {
    if (dropped != nullptr) *dropped = 0;

    const JsonValue root = parseJson(text);
    if (!root.isArray()) {
        // The file exists and does not parse. That is one dropped "row" as far
        // as the caller is concerned — enough for it to decide the index is
        // damaged rather than empty, and to keep a copy before overwriting it.
        if (dropped != nullptr) *dropped = 1;
        return {};
    }

    std::vector<JsonValue> rows;
    rows.reserve(root.items().size());
    for (const JsonValue& item : root.items()) {
        if (item.isObject()) {
            rows.push_back(item);
        } else if (dropped != nullptr) {
            ++*dropped;
        }
    }
    return rows;
}

}  // namespace livewall
