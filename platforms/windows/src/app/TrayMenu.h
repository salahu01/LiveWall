// Builds the notification-area menu.
//
// Split out from `AppHost` because Win32 menu construction is verbose in a way
// that would otherwise bury the ten lines of application logic in the host.
// The menu is rebuilt every time it opens, so an idle app does nothing at all —
// the same choice the macOS status menu makes.
//
// One structural difference from macOS worth noting: `NSMenuItem` carries a
// `representedObject`, so a menu item can hold the id of the wallpaper it
// selects. A Win32 menu item carries only a UINT command id, so the wallpaper
// list is addressed by index into a snapshot taken when the menu was built —
// `wallpaperIds` below. That snapshot is what the command handler resolves
// against, which also means a library that changed while the menu was open
// cannot select the wrong file.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace livewall {

class WallpaperEngine;

class TrayMenu {
public:
    struct Built {
        HMENU menu = nullptr;
        // Library item ids in the order they were added to the menu, so
        // IDM_WALLPAPER_FIRST + n resolves back to an id.
        std::vector<std::string> wallpaperIds;
    };

    // `importing` and `importPercent` drive the "Converting… 42%" item that
    // replaces "Add Video…" during an import.
    static Built build(WallpaperEngine& engine, bool importing, int importPercent);

    static void destroy(Built& built);
};

}  // namespace livewall
