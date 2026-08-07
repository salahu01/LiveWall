// On-disk store of converted wallpapers.
//
// Originals are never copied or modified — only the normalised output lives
// here, so the library size stays proportional to what actually plays.
//
// The index format is shared with the macOS and Windows ports on purpose: same
// field names, same uppercase-hyphenated ids, same ISO 8601 timestamps. The
// media files are not portable between them, but the index is, and that costs
// nothing to preserve.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/FitMode.h"
#include "support/Settings.h"

namespace livewall {

struct WallpaperItem {
    std::string id;
    std::string title;
    std::string filename;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::int64_t byteCount = 0;
    std::string addedAt;

    // Absent in an index written before 10-bit support; absent means 8-bit,
    // which is what those files are. Optional rather than defaulted so that a
    // future field cannot fail the whole array the way a required one would.
    int bitDepth = 8;

    // Linux-only, and optional for the same reason. The other two ports always
    // produce HEVC; here it depends on what the machine's FFmpeg build had, so
    // the answer is recorded rather than assumed. Empty means "assume HEVC",
    // which is right for an index written by the other two.
    std::string codec;

    std::string resolutionLabel() const;
    std::string sizeLabel() const;
};

class Library {
public:
    Library();

    const std::vector<WallpaperItem>& items() const { return items_; }

    // Re-reads the index from disk.
    //
    // Needed because `livewall add` transcodes in the *client* process, not in
    // the daemon: an import takes minutes and the daemon's single thread is
    // also every output's frame pump. The client writes the index and the
    // daemon reloads, which costs a file read on a command a user typed.
    void reload();

    // Decodes the index one entry at a time.
    //
    // Decoding all-or-nothing is the trap this avoids, and the macOS port
    // learned it the hard way: a single malformed entry — one bad id, one field
    // written by a future version — throws for the whole array and the library
    // silently reads as empty, which to the user looks like every wallpaper
    // they ever imported has vanished. One bad row should cost one row.
    static std::vector<WallpaperItem> decodeIndex(std::string_view json, int* dropped);
    static std::string encodeIndex(const std::vector<WallpaperItem>& items);

    std::string directory() const { return directory_; }
    std::string pathFor(const WallpaperItem& item) const;
    std::string destinationFor(const std::string& id) const;

    void add(const WallpaperItem& item);
    bool remove(const std::string& id);
    const WallpaperItem* find(const std::string& id) const;

    // Matches an id prefix, a full id, or a title. What `livewall play` takes,
    // because nobody is going to type a UUID.
    const WallpaperItem* resolve(std::string_view query) const;

    std::int64_t totalBytes() const;

    // --- settings -----------------------------------------------------------

    std::string selectedId() const;
    void setSelectedId(const std::string& id);

    std::string presetName() const;
    void setPresetName(const std::string& name);

    bool pauseOnBattery() const;
    void setPauseOnBattery(bool value);

    FitMode fitMode() const;
    void setFitMode(FitMode mode);

    int proceduralFps() const;
    std::string backendPreference() const;

private:
    void load();
    void save();

    std::vector<WallpaperItem> items_;
    std::string directory_;
    std::string indexPath_;
    Settings settings_;
};

}  // namespace livewall
