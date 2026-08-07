// Persisted preferences — the UserDefaults equivalent.
//
// Kept in $XDG_CONFIG_HOME/livewall/settings.json. Not dconf/GSettings: that
// would pull in GLib for six values, and would tie a Wayland tiling-WM user to
// a GNOME component they have no other reason to have installed. Not a
// dotfile-shaped INI either, because the JSON reader already exists for the
// index.
//
// Every setter writes the whole file. There are six settings and they change
// when a user runs a command, so anything cleverer would be machinery for its
// own sake.
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
    // The first four match the macOS and Windows ports exactly.
    static constexpr std::string_view kSelectedId = "selectedWallpaperId";
    static constexpr std::string_view kPreset = "importPresetName";
    static constexpr std::string_view kPauseOnBattery = "pauseOnBattery";
    static constexpr std::string_view kFitMode = "fitMode";
    // Linux-only. The procedural mode is not free here the way a CAGradientLayer
    // is on macOS — see README — so its cost is a setting rather than a
    // constant.
    static constexpr std::string_view kProceduralFps = "proceduralFps";
    // "auto", "x11" or "wayland". Overriding matters because a session can be
    // both, and which one the app picks changes whether occlusion works.
    static constexpr std::string_view kBackend = "backend";

private:
    void save() const;

    JsonValue root_ = JsonValue::object();
};

}  // namespace livewall
