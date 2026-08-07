// A small JSON reader and writer.
//
// Written rather than pulled in because the whole requirement is one array of
// flat objects — the library index — and a dependency for that would be the
// only dependency in the project.
//
// The property that matters is stated on `parseArrayLenient`: one malformed
// entry must cost one entry. The macOS side learned this the hard way; decoding
// straight into an array is all-or-nothing, so a single bad row makes the whole
// library read as empty, which to the user looks like every wallpaper they ever
// imported has vanished.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace livewall {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    explicit JsonValue(bool value) : type_(Type::Bool), boolean_(value) {}
    explicit JsonValue(double value) : type_(Type::Number), number_(value) {}
    explicit JsonValue(std::int64_t value)
        : type_(Type::Number), number_(static_cast<double>(value)) {}
    explicit JsonValue(int value) : type_(Type::Number), number_(value) {}
    explicit JsonValue(std::string value) : type_(Type::String), text_(std::move(value)) {}
    explicit JsonValue(const char* value) : type_(Type::String), text_(value) {}

    static JsonValue array() {
        JsonValue v;
        v.type_ = Type::Array;
        return v;
    }
    static JsonValue object() {
        JsonValue v;
        v.type_ = Type::Object;
        return v;
    }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }

    // Readers. Each returns `fallback` when the value is absent or of the wrong
    // type, so a caller never has to check twice.
    bool boolValue(bool fallback = false) const;
    double numberValue(double fallback = 0) const;
    std::int64_t intValue(std::int64_t fallback = 0) const;
    const std::string& stringValue() const;

    // Object access. `has` distinguishes "absent" from "present and null",
    // which is what makes an optional field (bitDepth) decodable.
    bool has(std::string_view key) const;
    const JsonValue& operator[](std::string_view key) const;

    const std::vector<JsonValue>& items() const { return array_; }
    void push(JsonValue value);
    void set(std::string key, JsonValue value);
    void erase(std::string_view key);

    std::string serialize(bool pretty = false, int indent = 0) const;

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0;
    std::string text_;
    std::vector<JsonValue> array_;
    // Ordered, so a written index has stable key order and diffs cleanly.
    std::map<std::string, JsonValue, std::less<>> members_;
};

// Returns a Null value when `text` is not valid JSON.
JsonValue parseJson(std::string_view text);

// Parses a top-level array and keeps only the elements that are objects.
// `dropped` reports how many were skipped; a caller uses that to tell "the
// library is empty" apart from "the index is damaged".
std::vector<JsonValue> parseArrayLenient(std::string_view text, int* dropped);

}  // namespace livewall
