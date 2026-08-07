#include "import/Library.h"

#include <windows.h>

#include <algorithm>

#include "support/Guid.h"
#include "support/Json.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

std::string isoTimestampNow() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    return format("%04d-%02d-%02dT%02d:%02d:%02dZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                  utc.wMinute, utc.wSecond);
}

}  // namespace

std::string WallpaperItem::resolutionLabel() const {
    return format("%d×%d · %d fps · %d-bit", width, height, fps, pixelBitDepth());
}

std::string WallpaperItem::sizeLabel() const { return formatBytes(byteCount); }

Library::Library() { load(); }

std::wstring Library::directory() const { return paths::libraryDirectory(); }

std::wstring Library::pathFor(const WallpaperItem& item) const {
    return paths::join(directory(), widen(item.filename));
}

std::wstring Library::destinationFor(const std::string& id) const {
    return paths::join(directory(), widen(id + ".mp4"));
}

const WallpaperItem* Library::item(const std::string& id) const {
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [&](const WallpaperItem& candidate) {
                                     return candidate.id == id;
                                 });
    return it == items_.end() ? nullptr : &*it;
}

Library::DecodeOutcome Library::decodeIndex(std::string_view json) {
    DecodeOutcome outcome;

    int dropped = 0;
    const std::vector<JsonValue> rows = parseArrayLenient(json, &dropped);
    outcome.dropped = dropped;

    for (const JsonValue& row : rows) {
        WallpaperItem item;
        item.id = row["id"].stringValue();
        item.filename = row["filename"].stringValue();

        // One bad row costs one row. Decoding straight into a typed array is
        // all-or-nothing — a single malformed entry, one field written by a
        // future version, and the whole library reads as empty, which to the
        // user looks like every wallpaper they ever imported has vanished.
        if (!isGuidString(item.id) || item.filename.empty()) {
            ++outcome.dropped;
            continue;
        }

        item.title = row["title"].stringValue();
        if (item.title.empty()) item.title = item.id;
        item.width = static_cast<int>(row["width"].intValue());
        item.height = static_cast<int>(row["height"].intValue());
        item.fps = static_cast<int>(row["fps"].intValue());
        item.byteCount = row["byteCount"].intValue();
        item.addedAt = row["addedAt"].stringValue();

        if (row.has("bitDepth") && row["bitDepth"].type() == JsonValue::Type::Number) {
            item.bitDepth = static_cast<int>(row["bitDepth"].intValue());
        }

        if (item.width <= 0 || item.height <= 0 || item.fps <= 0) {
            ++outcome.dropped;
            continue;
        }

        outcome.items.push_back(std::move(item));
    }

    return outcome;
}

std::string Library::encodeIndex(const std::vector<WallpaperItem>& items) {
    JsonValue array = JsonValue::array();
    for (const WallpaperItem& item : items) {
        JsonValue row = JsonValue::object();
        row.set("id", JsonValue(item.id));
        row.set("title", JsonValue(item.title));
        row.set("filename", JsonValue(item.filename));
        row.set("width", JsonValue(item.width));
        row.set("height", JsonValue(item.height));
        row.set("fps", JsonValue(item.fps));
        row.set("byteCount", JsonValue(static_cast<std::int64_t>(item.byteCount)));
        row.set("addedAt", JsonValue(item.addedAt));
        if (item.bitDepth.has_value()) row.set("bitDepth", JsonValue(*item.bitDepth));
        array.push(std::move(row));
    }
    return array.serialize(/*pretty=*/true);
}

void Library::load() {
    const std::wstring indexPath = paths::indexFile();
    const std::string text = paths::readFile(indexPath);
    if (text.empty()) return;

    DecodeOutcome outcome = decodeIndex(text);

    // A file that parses as JSON but yields nothing usable is more likely a bug
    // or a bad migration than an empty library, and `save()` would overwrite it
    // moments later. Keep a copy before that happens.
    if (outcome.items.empty() && outcome.dropped > 0) {
        const std::wstring backup = indexPath + L".corrupt";
        paths::removeFile(backup);
        CopyFileW(indexPath.c_str(), backup.c_str(), FALSE);
        Log::error("index unreadable — kept a copy at " + narrow(paths::filename(backup)));
    } else if (outcome.dropped > 0) {
        Log::error(format("skipped %d unreadable entries in the library index", outcome.dropped));
    }

    items_ = std::move(outcome.items);

    // Drop entries whose file vanished (manual delete, a half-finished
    // migration, a library folder restored without its index).
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [this](const WallpaperItem& item) {
                                    return !paths::fileExists(pathFor(item));
                                }),
                 items_.end());
}

void Library::save() const {
    paths::writeFileAtomically(paths::indexFile(), encodeIndex(items_));
}

void Library::add(WallpaperItem item) {
    if (item.addedAt.empty()) item.addedAt = isoTimestampNow();

    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const WallpaperItem& existing) {
                                    return existing.id == item.id;
                                }),
                 items_.end());
    items_.push_back(std::move(item));

    // Newest first, matching the tray menu's order.
    std::sort(items_.begin(), items_.end(),
              [](const WallpaperItem& a, const WallpaperItem& b) {
                  return a.addedAt > b.addedAt;
              });
    save();
}

void Library::remove(const std::string& id) {
    const WallpaperItem* existing = item(id);
    if (existing == nullptr) return;

    paths::removeFile(pathFor(*existing));
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const WallpaperItem& candidate) {
                                    return candidate.id == id;
                                }),
                 items_.end());

    if (selectedId() == id) setSelectedId({});
    save();
}

long long Library::totalBytes() const {
    long long total = 0;
    for (const WallpaperItem& item : items_) total += item.byteCount;
    return total;
}

std::string Library::selectedId() const {
    return settings_.stringValue(Settings::kSelectedId);
}

void Library::setSelectedId(const std::string& id) {
    if (id.empty()) {
        settings_.remove(Settings::kSelectedId);
    } else {
        settings_.setString(Settings::kSelectedId, id);
    }
}

const Transcoder::Preset& Library::preset() const {
    const std::string name =
        settings_.stringValue(Settings::kPreset, Transcoder::kBalanced.name);
    return *Transcoder::presetByName(name);
}

void Library::setPreset(const Transcoder::Preset& preset) {
    settings_.setString(Settings::kPreset, preset.name);
}

bool Library::pauseOnBattery() const {
    return settings_.boolValue(Settings::kPauseOnBattery, false);
}

void Library::setPauseOnBattery(bool value) {
    settings_.setBool(Settings::kPauseOnBattery, value);
}

FitMode Library::fitMode() const {
    return fitModeFromName(
        settings_.stringValue(Settings::kFitMode, std::string(fitModeName(kDefaultFitMode))));
}

void Library::setFitMode(FitMode mode) {
    settings_.setString(Settings::kFitMode, std::string(fitModeName(mode)));
}

}  // namespace livewall
