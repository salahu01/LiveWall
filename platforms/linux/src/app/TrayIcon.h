// The tray icon, over StatusNotifierItem.
//
// Not XEmbed. The old system tray protocol needs an X11 connection and does not
// exist under Wayland at all; SNI is DBus and works on both, and it is what
// KDE, GNOME (with the AppIndicator extension), XFCE, waybar, Polybar and
// most other bars actually consume today.
//
// What this deliberately does not have: a menu.
//
// A StatusNotifierItem's menu is not part of SNI — it is a second protocol,
// com.canonical.dbusmenu, whose GetLayout returns a recursively nested
// `(ia{sv}av)`. Supporting it means a general DBus marshaller, and this app's
// wrapper is a deliberately small one that handles a fixed set of shapes. The
// choice is therefore between a much larger DBus layer and a tray that does
// three things by click, and the CLI already does everything either way.
//
// So: left click cycles wallpapers, middle click cycles fit mode, and the
// tooltip title carries the status line. Everything else is `livewall`.
// The README lists this as a known limit rather than leaving it to be
// discovered.
#pragma once

#include <string>
#include <string_view>

namespace livewall {

class WallpaperEngine;

class TrayIcon {
public:
    // False when there is no session bus or no StatusNotifierWatcher on it,
    // which is normal on a tiling WM with no bar and is not an error.
    bool start(WallpaperEngine& engine);

    // Emits NewTitle/NewIcon if the state actually changed. Called from the
    // engine's state-change callback, which fires far more often than the
    // displayed text changes.
    void refresh();

    bool active() const { return registered_; }

    // Routed in by AppHost, so a watcher that starts after the app does — a
    // panel restart, or a bar launched later in the autostart batch — is picked
    // up rather than lost.
    void handleBusSignal(std::string_view interface, std::string_view member);

private:
    bool registerWithWatcher();
    std::string currentTitle() const;
    void cycleWallpaper();
    void cycleFitMode();

    WallpaperEngine* engine_ = nullptr;
    std::string serviceName_;
    std::string lastTitle_;
    bool registered_ = false;
    bool exported_ = false;
};

}  // namespace livewall
