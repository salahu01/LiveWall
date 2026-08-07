// Persisted preferences — the UserDefaults equivalent.
//
// Kept in settings.json beside the library rather than in the registry. Two
// reasons: the JSON reader already exists for the index, and a user who deletes
// %LOCALAPPDATA%\LiveWall should be rid of the app entirely rather than left
// with a registry key that quietly restores a wallpaper selection pointing at
// files that are gone.
//
// The one thing that does live in the registry is the Run key, because that is
// where Windows looks; see StartupItem.
//
// Every setter writes the whole file. There are six settings and they change
// when a user clicks a menu item, so anything cleverer would be machinery for
// its own sake.
#pragma once

#include <string>
#include <string_view>

#include "support/Json.h"

namespace livewall {

class Settings {
public:
    Settings();

    std::string stringValue(std::string_view key, std::string fallback = {}) const;
    bool boolValue(std::string_view key, bool fallback = false) const;
    long long intValue(std::string_view key, long long fallback = 0) const;

    void setString(std::string_view key, std::string value);
    void setBool(std::string_view key, bool value);
    void setInt(std::string_view key, long long value);
    void remove(std::string_view key);

    // Key names. Constants rather than literals at the call sites, so a typo in
    // one place cannot silently create a second setting that nothing reads.
    static constexpr std::string_view kSelectedId = "selectedWallpaperId";
    static constexpr std::string_view kPreset = "importPresetName";
    static constexpr std::string_view kPauseOnBattery = "pauseOnBattery";
    static constexpr std::string_view kFitMode = "fitMode";

private:
    void save() const;

    JsonValue root_ = JsonValue::object();
};

}  // namespace livewall
