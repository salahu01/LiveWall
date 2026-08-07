// Tray-only front end. There is no window anyone can see, no taskbar button and
// no timer of its own: the menu is rebuilt when it opens, so an idle app does
// nothing at all.
//
// The message-only window is the app's spine. Everything the OS needs to tell
// this process — power state, session lock, display changes, Explorer
// restarting, the tray icon being clicked — arrives as a message here, and the
// engine's timers hang off it too.
#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app/TrayMenu.h"
#include "app/WallpaperEngine.h"

namespace livewall {

class AppHost {
public:
    AppHost();
    ~AppHost();

    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;

    // Creates the window and the tray icon and runs the message loop until the
    // user quits. Returns the process exit code.
    int run();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    bool createWindow();
    bool addTrayIcon();
    void removeTrayIcon();
    void updateTrayTooltip();
    void showMenu();
    void onCommand(UINT command);

    void startImport();
    void finishImport();
    void showError(const std::string& message);

    static constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;
    // Posted by the import thread when it is done, so the result is handled on
    // the UI thread rather than from the worker.
    static constexpr UINT WM_IMPORT_FINISHED = WM_APP + 2;
    static constexpr UINT_PTR kTrayIconId = 1;

    HWND window_ = nullptr;
    HICON trayIcon_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;

    WallpaperEngine engine_;
    TrayMenu::Built menu_;

    // Import state. The conversion runs on its own thread — it takes tens of
    // seconds and the message loop has to stay responsive, or Windows marks the
    // process as not responding and the tray icon stops working.
    std::thread importThread_;
    std::atomic<bool> importing_{false};
    std::atomic<bool> importCancelled_{false};
    std::atomic<int> importPercent_{0};
    std::string importError_;
    std::string importItemId_;
    WallpaperItem importedItem_;
    bool importSucceeded_ = false;
};

}  // namespace livewall
