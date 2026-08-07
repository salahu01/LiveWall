// On-disk store of converted wallpapers.
//
// Originals are never copied or modified — only the normalised output lives
// here, so the library size stays proportional to what actually plays.
//
// The index format is byte-for-byte the one the macOS app writes, including the
// ISO 8601 timestamps and the optional `bitDepth` field. Nothing depends on
// that today, but the two apps are the same project and an index that only one
// of them can read would be a decision made by accident rather than on purpose.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "import/Transcoder.h"
#include "render/FitMode.h"
#include "support/Settings.h"

namespace livewall {

struct WallpaperItem {
    std::string id;        // GUID, uppercase hyphenated
    std::string title;
    std::string filename;  // relative to the library directory
    int width = 0;
    int height = 0;
    int fps = 0;
    long long byteCount = 0;
    std::string addedAt;   // ISO 8601
    // Optional so that an index written before 10-bit support still decodes.
    // Absent means 8-bit, which is what those files are.
    std::optional<int> bitDepth;

    int pixelBitDepth() const { return bitDepth.value_or(8); }
    std::string resolutionLabel() const;
    std::string sizeLabel() const;
};

class Library {
public:
    Library();

    const std::vector<WallpaperItem>& items() const { return items_; }
    const WallpaperItem* item(const std::string& id) const;

    std::wstring directory() const;
    std::wstring pathFor(const WallpaperItem& item) const;
    std::wstring destinationFor(const std::string& id) const;

    void add(WallpaperItem item);
    void remove(const std::string& id);

    long long totalBytes() const;

    // Settings, stored in settings.json.
    std::string selectedId() const;
    void setSelectedId(const std::string& id);  // empty clears the selection

    const Transcoder::Preset& preset() const;
    void setPreset(const Transcoder::Preset& preset);

    bool pauseOnBattery() const;
    void setPauseOnBattery(bool value);

    FitMode fitMode() const;
    void setFitMode(FitMode mode);

    // Decodes an index document. Public and static so the test suite can
    // exercise it without touching the disk, which is the same shape the macOS
    // `Library.decodeIndex` takes and for the same reason.
    struct DecodeOutcome {
        std::vector<WallpaperItem> items;
        int dropped = 0;
    };
    static DecodeOutcome decodeIndex(std::string_view json);
    static std::string encodeIndex(const std::vector<WallpaperItem>& items);

private:
    void load();
    void save() const;

    std::vector<WallpaperItem> items_;
    mutable Settings settings_;
};

}  // namespace livewall
