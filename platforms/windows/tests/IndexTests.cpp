// Library index decoding and the JSON reader under it. Mirrors
// LibraryIndexTests.swift.
//
// The property under test is the one the macOS side learned the hard way: one
// malformed entry must cost one entry. Decoding all-or-nothing means a single
// bad row makes the whole library read as empty, which to the user looks like
// every wallpaper they ever imported has vanished.

#include "TestHarness.h"

#include "import/Library.h"
#include "support/Json.h"

using namespace livewall;

namespace {

const char* kValidRow = R"([
  {
    "id": "3F2504E0-4F89-41D3-9A0C-0305E82C3301",
    "title": "Aurora",
    "filename": "3F2504E0-4F89-41D3-9A0C-0305E82C3301.mp4",
    "width": 1920,
    "height": 1080,
    "fps": 24,
    "byteCount": 4194304,
    "addedAt": "2026-01-02T03:04:05Z",
    "bitDepth": 10
  }
])";

}  // namespace

TEST_CASE("a well formed index decodes") {
    const auto outcome = Library::decodeIndex(kValidRow);
    CHECK_EQ(outcome.items.size(), 1ull);
    CHECK_EQ(outcome.dropped, 0);

    const WallpaperItem& item = outcome.items[0];
    CHECK_EQ(item.title, std::string("Aurora"));
    CHECK_EQ(item.width, 1920);
    CHECK_EQ(item.height, 1080);
    CHECK_EQ(item.fps, 24);
    CHECK_EQ(item.byteCount, 4194304LL);
    CHECK_EQ(item.pixelBitDepth(), 10);
}

TEST_CASE("a missing bitDepth means 8-bit rather than a decode failure") {
    // Index files written before 10-bit support have no such field. A
    // non-optional addition would fail the whole array and silently empty the
    // library.
    const char* json = R"([
      {
        "id": "3F2504E0-4F89-41D3-9A0C-0305E82C3301",
        "title": "Old",
        "filename": "old.mp4",
        "width": 1280, "height": 720, "fps": 20,
        "byteCount": 1024, "addedAt": "2025-01-01T00:00:00Z"
      }
    ])";
    const auto outcome = Library::decodeIndex(json);
    CHECK_EQ(outcome.items.size(), 1ull);
    CHECK_EQ(outcome.items[0].pixelBitDepth(), 8);
}

TEST_CASE("one bad row costs one row") {
    const char* json = R"([
      { "id": "not-a-guid", "filename": "x.mp4", "width": 1, "height": 1, "fps": 1 },
      {
        "id": "3F2504E0-4F89-41D3-9A0C-0305E82C3301",
        "title": "Good", "filename": "good.mp4",
        "width": 1920, "height": 1080, "fps": 24,
        "byteCount": 10, "addedAt": "2026-01-01T00:00:00Z"
      }
    ])";
    const auto outcome = Library::decodeIndex(json);
    CHECK_EQ(outcome.items.size(), 1ull);
    CHECK_EQ(outcome.items[0].title, std::string("Good"));
    CHECK_EQ(outcome.dropped, 1);
}

TEST_CASE("a row with impossible dimensions is dropped rather than played") {
    const char* json = R"([
      {
        "id": "3F2504E0-4F89-41D3-9A0C-0305E82C3301",
        "title": "Broken", "filename": "broken.mp4",
        "width": 0, "height": 0, "fps": 0,
        "byteCount": 0, "addedAt": "2026-01-01T00:00:00Z"
      }
    ])";
    const auto outcome = Library::decodeIndex(json);
    CHECK_EQ(outcome.items.size(), 0ull);
    CHECK_EQ(outcome.dropped, 1);
}

TEST_CASE("a non-object element is dropped, not fatal") {
    const char* json = R"(["nonsense", 42, null])";
    const auto outcome = Library::decodeIndex(json);
    CHECK_EQ(outcome.items.size(), 0ull);
    CHECK_EQ(outcome.dropped, 3);
}

TEST_CASE("an unparseable index reports a drop, so the caller keeps a backup") {
    const auto outcome = Library::decodeIndex("{ this is not json");
    CHECK_EQ(outcome.items.size(), 0ull);
    CHECK(outcome.dropped > 0);
}

TEST_CASE("an empty array is an empty library, not a damaged one") {
    const auto outcome = Library::decodeIndex("[]");
    CHECK_EQ(outcome.items.size(), 0ull);
    CHECK_EQ(outcome.dropped, 0);
}

TEST_CASE("an index round-trips through encode and decode") {
    const auto decoded = Library::decodeIndex(kValidRow);
    const std::string encoded = Library::encodeIndex(decoded.items);
    const auto again = Library::decodeIndex(encoded);

    CHECK_EQ(again.items.size(), 1ull);
    CHECK_EQ(again.dropped, 0);
    CHECK_EQ(again.items[0].id, decoded.items[0].id);
    CHECK_EQ(again.items[0].title, decoded.items[0].title);
    CHECK_EQ(again.items[0].byteCount, decoded.items[0].byteCount);
    CHECK_EQ(again.items[0].pixelBitDepth(), 10);
}

// ---------------------------------------------------------------------------
// The JSON reader itself
// ---------------------------------------------------------------------------

TEST_CASE("strings, numbers, booleans and null parse") {
    const JsonValue value = parseJson(
        R"({"s":"text","n":-12.5,"i":42,"t":true,"f":false,"z":null})");
    CHECK(value.isObject());
    CHECK_EQ(value["s"].stringValue(), std::string("text"));
    CHECK_NEAR(value["n"].numberValue(), -12.5, 0.0001);
    CHECK_EQ(value["i"].intValue(), 42LL);
    CHECK_EQ(value["t"].boolValue(), true);
    CHECK_EQ(value["f"].boolValue(true), false);
    CHECK(value.has("z"));
    CHECK(value["z"].isNull());
}

TEST_CASE("an absent key reads as its fallback rather than crashing") {
    const JsonValue value = parseJson(R"({"a":1})");
    CHECK(!value.has("b"));
    CHECK_EQ(value["b"].intValue(7), 7LL);
    CHECK_EQ(value["b"].stringValue(), std::string());
}

TEST_CASE("escapes and UTF-8 survive a round trip") {
    const JsonValue parsed = parseJson(R"({"k":"a\"b\\c\ndéA"})");
    CHECK_EQ(parsed["k"].stringValue(), std::string("a\"b\\c\nd\xc3\xa9" "A"));

    JsonValue rebuilt = JsonValue::object();
    rebuilt.set("k", JsonValue(parsed["k"].stringValue()));
    const JsonValue again = parseJson(rebuilt.serialize());
    CHECK_EQ(again["k"].stringValue(), parsed["k"].stringValue());
}

TEST_CASE("a surrogate pair becomes one code point") {
    // U+1F600 as \uD83D\uDE00, which is how a JSON writer that escapes
    // non-ASCII emits it. Decoding the halves separately would produce two
    // unpaired surrogates that no UTF-8 reader will accept.
    const JsonValue value = parseJson(R"({"k":"\uD83D\uDE00"})");
    CHECK_EQ(value["k"].stringValue(), std::string("\xf0\x9f\x98\x80"));
}

TEST_CASE("trailing content is rejected rather than half-accepted") {
    CHECK(parseJson("{} garbage").isNull());
    CHECK(parseJson("[1,2]]").isNull());
}

TEST_CASE("whole numbers serialise without a decimal point") {
    JsonValue value = JsonValue::object();
    value.set("w", JsonValue(1920));
    CHECK(value.serialize().find("1920") != std::string::npos);
    CHECK(value.serialize().find("1920.0") == std::string::npos);
}

TEST_CASE("erase removes a key entirely") {
    JsonValue value = JsonValue::object();
    value.set("a", JsonValue(1));
    value.set("b", JsonValue(2));
    value.erase("a");
    CHECK(!value.has("a"));
    CHECK(value.has("b"));
}
