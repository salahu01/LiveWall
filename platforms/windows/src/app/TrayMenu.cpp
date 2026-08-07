#include "app/TrayMenu.h"

#include "resource.h"

#include "app/MonitorController.h"
#include "app/WallpaperEngine.h"
#include "import/CodecSupport.h"
#include "render/DesktopHost.h"
#include "support/Footprint.h"
#include "support/StartupItem.h"
#include "support/Strings.h"

namespace livewall {
namespace {

void appendText(HMENU menu, UINT id, const std::string& text, bool enabled = true,
                bool checked = false) {
    const std::wstring wide = widen(text);

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
    info.wID = id;
    info.dwTypeData = const_cast<wchar_t*>(wide.c_str());
    info.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0u);
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

void appendSeparator(HMENU menu) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_FTYPE;
    info.fType = MFT_SEPARATOR;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

void appendSubmenu(HMENU menu, HMENU submenu, const std::string& text) {
    const std::wstring wide = widen(text);

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_STATE;
    info.dwTypeData = const_cast<wchar_t*>(wide.c_str());
    info.hSubMenu = submenu;
    info.fState = MFS_ENABLED;
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
}

// Scaling submenu. Each mode's label states what it costs for the wallpaper
// actually selected on the display the mouse is on — the aspect mismatch is
// otherwise invisible, and it is the whole reason this submenu exists.
//
// Win32 menus have no tooltips, which is where the macOS version puts this, so
// the sentence goes into the label itself.
HMENU buildScalingMenu(WallpaperEngine& engine) {
    HMENU submenu = CreatePopupMenu();

    const std::string selectedId = engine.library().selectedId();
    const WallpaperItem* selected =
        selectedId.empty() ? nullptr : engine.library().item(selectedId);
    const MonitorController* controller = engine.primaryController();

    const FitMode current = engine.library().fitMode();

    for (const FitMode mode : kAllFitModes) {
        std::string label = std::string(fitModeTitle(mode)) + " — " +
                            std::string(fitModeTradeoff(mode));

        if (selected != nullptr && controller != nullptr) {
            const std::string effect =
                fitModeEffect(mode, selected->width, selected->height, controller->width(),
                              controller->height());
            label += effect.empty() ? "  (same shape as this display)" : "  " + effect;
        }

        appendText(submenu, IDM_FIT_FIRST + static_cast<UINT>(mode), label, true,
                   mode == current);
    }

    if (selected == nullptr) {
        appendSeparator(submenu);
        appendText(submenu, 0, "Applies to video wallpapers", /*enabled=*/false);
    }
    return submenu;
}

HMENU buildQualityMenu(WallpaperEngine& engine) {
    HMENU submenu = CreatePopupMenu();

    const std::string currentName = engine.library().preset().name;
    const auto presets = Transcoder::allPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
        const Transcoder::Preset* preset = presets[i];
        appendText(submenu, IDM_PRESET_FIRST + static_cast<UINT>(i),
                   std::string(preset->name) + " — " + preset->summary(), true,
                   currentName == preset->name);
    }

    // What this machine can actually do, when it is less than the preset asks
    // for. On macOS this never comes up — HEVC Main10 is guaranteed — so there
    // is nothing equivalent in that menu.
    const std::string fallback = CodecSupport::fallbackExplanation();
    if (!fallback.empty()) {
        appendSeparator(submenu);
        appendText(submenu, 0, fallback, /*enabled=*/false);
    }
    return submenu;
}

}  // namespace

TrayMenu::Built TrayMenu::build(WallpaperEngine& engine, bool importing, int importPercent) {
    Built built;
    built.menu = CreatePopupMenu();
    HMENU menu = built.menu;

    // A tray-only app has no title bar and no taskbar button, so the menu is the
    // only place its name appears.
    appendText(menu, 0, "LiveWall " VER_PRODUCTVERSION_STR, /*enabled=*/false);
    appendText(menu, 0, engine.statusLine(), /*enabled=*/false);
    appendText(menu, 0, "Memory: " + Footprint::formatted(), /*enabled=*/false);

    if (DesktopHost::usingFallbackParent()) {
        appendText(menu, 0, "Drawing over desktop icons — Explorer's layout was not as expected",
                   /*enabled=*/false);
    }

    appendSeparator(menu);

    const std::string selectedId = engine.library().selectedId();
    appendText(menu, IDM_PROCEDURAL, "Procedural (lightest)", true, selectedId.empty());

    const auto& items = engine.library().items();
    if (!items.empty()) {
        appendSeparator(menu);
        const size_t capacity = IDM_WALLPAPER_LAST - IDM_WALLPAPER_FIRST;
        for (size_t i = 0; i < items.size() && i < capacity; ++i) {
            const WallpaperItem& item = items[i];
            // Win32 has no tooltips on menu items, so the resolution and size
            // that macOS puts in one go on the label here.
            const std::string label =
                item.title + "   " + item.resolutionLabel() + " · " + item.sizeLabel();
            appendText(menu, IDM_WALLPAPER_FIRST + static_cast<UINT>(i), label, true,
                       item.id == selectedId);
            built.wallpaperIds.push_back(item.id);
        }
    }

    appendSeparator(menu);

    if (importing) {
        appendText(menu, 0, format("Converting… %d%%", importPercent), /*enabled=*/false);
    } else {
        appendText(menu, IDM_ADD_VIDEO, "Add Video…");
    }

    appendSubmenu(menu, buildQualityMenu(engine), "Import Quality");
    appendSubmenu(menu, buildScalingMenu(engine), "Scaling");

    appendText(menu, IDM_PAUSE_ON_BATTERY, "Pause on Battery", true,
               engine.library().pauseOnBattery());
    appendText(menu, IDM_START_AT_LOGIN, "Start with Windows", true, StartupItem::isEnabled());

    if (!selectedId.empty() && engine.library().item(selectedId) != nullptr) {
        appendText(menu, IDM_REMOVE_CURRENT, "Remove Current Wallpaper");
    }
    appendText(menu, IDM_REVEAL_LIBRARY, "Open Library Folder");

    appendSeparator(menu);
    appendText(menu, IDM_QUIT, "Quit LiveWall");

    return built;
}

void TrayMenu::destroy(Built& built) {
    if (built.menu != nullptr) {
        // Destroying the popup destroys its submenus with it.
        DestroyMenu(built.menu);
        built.menu = nullptr;
    }
    built.wallpaperIds.clear();
}

}  // namespace livewall
