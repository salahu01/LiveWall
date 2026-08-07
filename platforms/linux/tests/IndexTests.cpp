// Index decoding, and the JSON underneath it.
//
// The property being defended: one malformed entry costs one entry. The macOS
// port learned this the hard way — decoding straight into an array is
// all-or-nothing, so a single bad row makes the whole library read as empty,
// which to the user looks like every wallpaper they ever imported has vanished.
#include "Testing.h"

#include "import/Library.h"
#include "support/Guid.h"
#include "support/Json.h"
#include "support/Strings.h"

using namespace livewall;

namespace {

std::string oneGoodRow() {
    return R"([{"id":"3F2504E0-4F89-41D3-9A0C-0305E82C3301","title":"Aurora",)"
           R"("filename":"aurora.mp4","width":1920,"height":1080,"fps":24,)"
           R"("byteCount":12345678,"addedAt":"2026-01-02T03:04:05Z","bitDepth":10}])";
}

}  // namespace

TEST(index, decodesAWellFormedRow) {
    int dropped = -1;
    const std::vector<WallpaperItem> items = Library::decodeIndex(oneGoodRow(), &dropped);

    EXPECT_EQ(items.size(), size_t{1});
    EXPECT_EQ(dropped, 0);
    if (items.empty()) return;

    EXPECT_EQ(items[0].title, std::string("Aurora"));
    EXPECT_EQ(items[0].width, 1920);
    EXPECT_EQ(items[0].fps, 24);
    EXPECT_EQ(items[0].bitDepth, 10);
    EXPECT_EQ(items[0].byteCount, std::int64_t{12345678});
}

TEST(index, absentBitDepthMeansEight) {
    // An index written before 10-bit support. A required field would have
    // failed the whole array.
    const std::string json =
        R"([{"id":"A","title":"Old","filename":"old.mp4","width":1280,"height":720,"fps":24}])";
    const std::vector<WallpaperItem> items = Library::decodeIndex(json, nullptr);
    EXPECT_EQ(items.size(), size_t{1});
    if (!items.empty()) EXPECT_EQ(items[0].bitDepth, 8);
}

TEST(index, absentCodecIsEmptyRatherThanWrong) {
    // An index written by the macOS or Windows port has no codec field. Empty
    // means "assume HEVC", which is what those files are.
    const std::vector<WallpaperItem> items = Library::decodeIndex(oneGoodRow(), nullptr);
    EXPECT_EQ(items.size(), size_t{1});
    if (!items.empty()) EXPECT_TRUE(items[0].codec.empty());
}

TEST(index, oneBadRowCostsOneRow) {
    const std::string json =
        R"([{"id":"A","title":"Good","filename":"a.mp4","width":1920,"height":1080,"fps":24},)"
        R"(42,)"
        R"({"id":"B","title":"Also good","filename":"b.mp4","width":1280,"height":720,"fps":20}])";

    int dropped = 0;
    const std::vector<WallpaperItem> items = Library::decodeIndex(json, &dropped);
    EXPECT_EQ(items.size(), size_t{2});
    EXPECT_EQ(dropped, 1);
}

TEST(index, rowsMissingRequiredFieldsAreDroppedNotKeptEmpty) {
    // An entry with no filename cannot be played or deleted. Keeping it would
    // put a row in the list that does nothing when clicked.
    const std::string json =
        R"([{"id":"A","title":"No file","width":1920,"height":1080,"fps":24},)"
        R"({"id":"B","title":"Fine","filename":"b.mp4","width":1280,"height":720,"fps":20}])";

    int dropped = 0;
    const std::vector<WallpaperItem> items = Library::decodeIndex(json, &dropped);
    EXPECT_EQ(items.size(), size_t{1});
    EXPECT_EQ(dropped, 1);
    if (!items.empty()) EXPECT_EQ(items[0].title, std::string("Fine"));
}

TEST(index, unparseableFileReportsDamageRatherThanEmptiness) {
    // The distinction the caller acts on: "the library is empty" is normal,
    // "the index is damaged" means keep a copy before overwriting it.
    int dropped = 0;
    const std::vector<WallpaperItem> items = Library::decodeIndex("this is not json", &dropped);
    EXPECT_TRUE(items.empty());
    EXPECT_TRUE(dropped > 0);
}

TEST(index, anEmptyArrayIsEmptyAndUndamaged) {
    int dropped = -1;
    const std::vector<WallpaperItem> items = Library::decodeIndex("[]", &dropped);
    EXPECT_TRUE(items.empty());
    EXPECT_EQ(dropped, 0);
}

TEST(index, unknownFieldsAreIgnoredNotFatal) {
    // Forward compatibility: an index written by a later version must still
    // decode here, or an upgrade-then-downgrade empties the library.
    const std::string json =
        R"([{"id":"A","title":"Future","filename":"a.mp4","width":1920,"height":1080,)"
        R"("fps":24,"hdrMetadata":{"maxLuminance":1000},"tags":["ambient"]}])";
    const std::vector<WallpaperItem> items = Library::decodeIndex(json, nullptr);
    EXPECT_EQ(items.size(), size_t{1});
}

TEST(index, roundTripsThroughEncode) {
    const std::vector<WallpaperItem> original = Library::decodeIndex(oneGoodRow(), nullptr);
    const std::vector<WallpaperItem> again =
        Library::decodeIndex(Library::encodeIndex(original), nullptr);

    EXPECT_EQ(again.size(), original.size());
    if (again.empty() || original.empty()) return;
    EXPECT_EQ(again[0].id, original[0].id);
    EXPECT_EQ(again[0].title, original[0].title);
    EXPECT_EQ(again[0].bitDepth, original[0].bitDepth);
    EXPECT_EQ(again[0].byteCount, original[0].byteCount);
    EXPECT_EQ(again[0].addedAt, original[0].addedAt);
}

TEST(index, titlesWithQuotesAndNewlinesSurvive) {
    std::vector<WallpaperItem> items(1);
    items[0].id = "A";
    items[0].filename = "a.mp4";
    items[0].width = 1920;
    items[0].height = 1080;
    items[0].fps = 24;
    items[0].title = "He said \"hi\"\nand left\ttab";

    const std::vector<WallpaperItem> again =
        Library::decodeIndex(Library::encodeIndex(items), nullptr);
    EXPECT_EQ(again.size(), size_t{1});
    if (!again.empty()) EXPECT_EQ(again[0].title, items[0].title);
}

TEST(index, nonAsciiTitlesStayUtf8) {
    std::vector<WallpaperItem> items(1);
    items[0].id = "A";
    items[0].filename = "a.mp4";
    items[0].width = 1920;
    items[0].height = 1080;
    items[0].fps = 24;
    items[0].title = "오로라 — aurore — 極光";

    const std::string encoded = Library::encodeIndex(items);
    // Passed through rather than expanded into \u escapes, so the file stays
    // readable to a person who opens it.
    EXPECT_TRUE(encoded.find("오로라") != std::string::npos);

    const std::vector<WallpaperItem> again = Library::decodeIndex(encoded, nullptr);
    EXPECT_EQ(again.size(), size_t{1});
    if (!again.empty()) EXPECT_EQ(again[0].title, items[0].title);
}

// --- the JSON layer ---------------------------------------------------------

TEST(json, parsesEscapesAndSurrogatePairs) {
    const JsonValue value = parseJson(R"({"a":"éA","b":"😀"})");
    EXPECT_TRUE(value.isObject());
    EXPECT_EQ(value["a"].stringValue(), std::string("éA"));
    // U+1F600, which only decodes correctly if the low surrogate is consumed.
    EXPECT_EQ(value["b"].stringValue(), std::string("\xF0\x9F\x98\x80"));
}

TEST(json, rejectsTrailingContent) {
    EXPECT_TRUE(parseJson("{} garbage").isNull());
    EXPECT_TRUE(parseJson("[1,2,3]]").isNull());
}

TEST(json, wholeNumbersSerialiseWithoutADecimalPoint) {
    JsonValue object = JsonValue::object();
    object.set("width", JsonValue(1920));
    EXPECT_TRUE(object.serialize().find("1920") != std::string::npos);
    EXPECT_TRUE(object.serialize().find("1920.0") == std::string::npos);
}

TEST(json, distinguishesAbsentFromNull) {
    const JsonValue value = parseJson(R"({"present":null})");
    EXPECT_TRUE(value.has("present"));
    EXPECT_FALSE(value.has("absent"));
}

TEST(json, deeplyNestedInputDoesNotRecurseWithoutBound) {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    EXPECT_TRUE(parseJson(deep).isNull());
}

// --- ids and formatting -----------------------------------------------------

TEST(guid, generatesTheCanonicalShape) {
    const std::string id = newGuidString();
    EXPECT_EQ(id.size(), size_t{36});
    EXPECT_TRUE(isGuidString(id));
    // Version 4, variant 1 — so an id from this port is the same shape as one
    // from Foundation's UUID() or Windows's CoCreateGuid.
    EXPECT_EQ(id[14], '4');
    EXPECT_TRUE(id[19] == '8' || id[19] == '9' || id[19] == 'A' || id[19] == 'B');
}

TEST(guid, distinctAcrossCalls) {
    EXPECT_TRUE(newGuidString() != newGuidString());
}

TEST(guid, rejectsWrongShapes) {
    EXPECT_FALSE(isGuidString(""));
    EXPECT_FALSE(isGuidString("3F2504E0-4F89-41D3-9A0C-0305E82C330"));
    EXPECT_FALSE(isGuidString("{3F2504E0-4F89-41D3-9A0C-0305E82C3301}"));
    EXPECT_FALSE(isGuidString("3F2504E0X4F89-41D3-9A0C-0305E82C3301"));
}

TEST(strings, formatsBytesTheWayFileManagersDo) {
    EXPECT_EQ(formatBytes(999), std::string("999 bytes"));
    EXPECT_EQ(formatBytes(1000), std::string("1.0 KB"));
    EXPECT_EQ(formatBytes(12400000), std::string("12.4 MB"));
    // No decimal above 100, which is where the extra digit stops meaning
    // anything.
    EXPECT_EQ(formatBytes(418000000), std::string("418 MB"));
}

TEST(strings, iso8601IsSortableAndZulu) {
    const std::string now = iso8601Now();
    EXPECT_EQ(now.size(), size_t{20});
    EXPECT_EQ(now.back(), 'Z');
    EXPECT_EQ(now[4], '-');
    EXPECT_EQ(now[10], 'T');
}

TEST(strings, splitDropsEmptyFields) {
    const std::vector<std::string> words = split("  play   abc  ", ' ');
    EXPECT_EQ(words.size(), size_t{2});
    if (words.size() >= 2) {
        EXPECT_EQ(words[0], std::string("play"));
        EXPECT_EQ(words[1], std::string("abc"));
    }
}
