#include "import/Library.h"

#include <algorithm>

#include "import/Transcoder.h"
#include "support/Guid.h"
#include "support/Json.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// An entry with no id, no filename or no dimensions is not something the app
// can play or delete, so it is one of the rows the lenient decoder drops.
bool isUsable(const WallpaperItem& item) {
    return !item.id.empty() && !item.filename.empty() && item.width > 0 && item.height > 0;
}

}  // namespace

std::string WallpaperItem::resolutionLabel() const {
    std::string label = format("%d×%d · %d fps · %d-bit", width, height, fps, bitDepth);
    if (!codec.empty() && codec != "hevc") label += " · " + codec;
    return label;
}

std::string WallpaperItem::sizeLabel() const { return formatBytes(byteCount); }

Library::Library() {
    directory_ = paths::libraryDirectory();
    indexPath_ = paths::indexFile();
    load();
}

std::vector<WallpaperItem> Library::decodeIndex(std::string_view json, int* dropped) {
    int skipped = 0;
    const std::vector<JsonValue> rows = parseArrayLenient(json, &skipped);

    std::vector<WallpaperItem> items;
    items.reserve(rows.size());

    for (const JsonValue& row : rows) {
        WallpaperItem item;
        item.id = row["id"].stringValue();
        item.title = row["title"].stringValue();
        item.filename = row["filename"].stringValue();
        item.width = static_cast<int>(row["width"].intValue());
        item.height = static_cast<int>(row["height"].intValue());
        item.fps = static_cast<int>(row["fps"].intValue());
        item.byteCount = row["byteCount"].intValue();
        item.addedAt = row["addedAt"].stringValue();
        // Absent means 8-bit. `has` rather than a zero check, so a genuine
        // future value of 0 would be preserved rather than rewritten.
        item.bitDepth = row.has("bitDepth") ? static_cast<int>(row["bitDepth"].intValue()) : 8;
        item.codec = row["codec"].stringValue();

        if (!isUsable(item)) {
            ++skipped;
            continue;
        }
        items.push_back(std::move(item));
    }

    if (dropped != nullptr) *dropped = skipped;
    return items;
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
        row.set("byteCount", JsonValue(item.byteCount));
        row.set("addedAt", JsonValue(item.addedAt));
        row.set("bitDepth", JsonValue(item.bitDepth));
        if (!item.codec.empty()) row.set("codec", JsonValue(item.codec));
        array.push(std::move(row));
    }
    return array.serialize(true) + "\n";
}

void Library::reload() {
    items_.clear();
    load();
}

void Library::load() {
    const std::string text = paths::readFile(indexPath_);
    if (text.empty()) return;

    int dropped = 0;
    items_ = decodeIndex(text, &dropped);

    // A file that parses but yields nothing usable is more likely a bug or a
    // bad migration than an empty library, and the next save() would overwrite
    // it moments later. Keep a copy before that happens.
    if (items_.empty() && dropped > 0) {
        const std::string backup = indexPath_ + ".corrupt";
        paths::removeFile(backup);
        paths::writeFileAtomically(backup, text);
        Log::error("index unreadable — kept a copy at " + paths::filename(backup));
    } else if (dropped > 0) {
        Log::error(format("skipped %d unreadable entries in the library index", dropped));
    }

    // Drop entries whose file vanished (manual delete, a half-finished
    // migration, a library directory restored without its contents).
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [this](const WallpaperItem& item) {
                                    return !paths::fileExists(pathFor(item));
                                }),
                 items_.end());
    if (items_.size() != before) {
        Log::info(format("%zu library entries had no file on disk", before - items_.size()));
    }
}

void Library::save() { paths::writeFileAtomically(indexPath_, encodeIndex(items_)); }

std::string Library::pathFor(const WallpaperItem& item) const {
    return paths::join(directory_, item.filename);
}

std::string Library::destinationFor(const std::string& id) const {
    return paths::join(directory_, id + ".mp4");
}

void Library::add(const WallpaperItem& item) {
    remove(item.id);
    items_.push_back(item);
    // Newest first, which is the order every list in the UI wants.
    std::sort(items_.begin(), items_.end(),
              [](const WallpaperItem& a, const WallpaperItem& b) { return a.addedAt > b.addedAt; });
    save();
}

bool Library::remove(const std::string& id) {
    const auto hit = std::find_if(items_.begin(), items_.end(),
                                  [&id](const WallpaperItem& item) { return item.id == id; });
    if (hit == items_.end()) return false;

    paths::removeFile(pathFor(*hit));
    items_.erase(hit);
    if (selectedId() == id) setSelectedId({});
    save();
    return true;
}

const WallpaperItem* Library::find(const std::string& id) const {
    for (const WallpaperItem& item : items_) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

const WallpaperItem* Library::resolve(std::string_view query) const {
    if (query.empty()) return nullptr;

    // Exact id first, so a full id can never be shadowed by a title that
    // happens to contain it.
    for (const WallpaperItem& item : items_) {
        if (item.id == query) return &item;
    }
    for (const WallpaperItem& item : items_) {
        if (equalsIgnoreCase(item.title, query)) return &item;
    }
    // A unique id prefix. Ambiguity is refused rather than resolved to the
    // first match: deleting the wrong wallpaper is not recoverable.
    const WallpaperItem* prefixMatch = nullptr;
    for (const WallpaperItem& item : items_) {
        if (!startsWith(item.id, query)) continue;
        if (prefixMatch != nullptr) return nullptr;
        prefixMatch = &item;
    }
    return prefixMatch;
}

std::int64_t Library::totalBytes() const {
    std::int64_t total = 0;
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

std::string Library::presetName() const {
    return settings_.stringValue(Settings::kPreset, Transcoder::defaultPreset().name);
}

void Library::setPresetName(const std::string& name) {
    settings_.setString(Settings::kPreset, name);
}

bool Library::pauseOnBattery() const { return settings_.boolValue(Settings::kPauseOnBattery); }

void Library::setPauseOnBattery(bool value) {
    settings_.setBool(Settings::kPauseOnBattery, value);
}

FitMode Library::fitMode() const {
    return fitModeFromString(settings_.stringValue(Settings::kFitMode, "fill"));
}

void Library::setFitMode(FitMode mode) {
    settings_.setString(Settings::kFitMode, std::string(fitModeToString(mode)));
}

int Library::proceduralFps() const {
    const long long value = settings_.intValue(Settings::kProceduralFps, 10);
    // Clamped rather than trusted. This number is a wakeup rate, and a
    // hand-edited settings.json with 240 in it would turn the cheapest mode
    // into the most expensive one.
    return static_cast<int>(std::clamp<long long>(value, 1, 60));
}

std::string Library::backendPreference() const {
    return settings_.stringValue(Settings::kBackend, "auto");
}

}  // namespace livewall
