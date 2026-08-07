#include "app/AppHost.h"

#include <commdlg.h>
#include <shellapi.h>

#include <cwchar>
#include <iterator>

#include "resource.h"

#include "support/Guid.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/StartupItem.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr wchar_t kWindowClass[] = L"LiveWallHostWindow";
constexpr wchar_t kWindowTitle[] = L"LiveWall";

// Every container Media Foundation can read, plus a catch-all. Deliberately
// wider than what the app can transcode well — the transcoder gives a specific
// error for a file it cannot read, which is more useful than the picker
// pretending the file does not exist.
constexpr wchar_t kFileFilter[] =
    L"Video files\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.mpg;*.mpeg;*.ts\0"
    L"All files\0*.*\0";

}  // namespace

AppHost::AppHost() = default;

AppHost::~AppHost() {
    importCancelled_ = true;
    if (importThread_.joinable()) importThread_.join();
    TrayMenu::destroy(menu_);
    removeTrayIcon();
    if (trayIcon_ != nullptr) DestroyIcon(trayIcon_);
}

int AppHost::run() {
    if (!createWindow()) return 1;

    StartupItem::reconcile();

    if (!addTrayIcon()) {
        Log::error("could not add the tray icon — the app would have no interface");
        return 1;
    }

    engine_.onStateChange = [this] { updateTrayTooltip(); };
    engine_.start(window_);
    updateTrayTooltip();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    engine_.stop();
    return static_cast<int>(message.wParam);
}

bool AppHost::createWindow() {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &AppHost::windowProc;
    description.hInstance = GetModuleHandleW(nullptr);
    description.lpszClassName = kWindowClass;
    if (RegisterClassExW(&description) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log::error("could not register the host window class: " + Log::lastError());
        return false;
    }

    // A real window rather than HWND_MESSAGE. A message-only window is not in
    // the window hierarchy, and several of the notifications this app depends
    // on — WM_DISPLAYCHANGE, WM_SETTINGCHANGE and the shell's TaskbarCreated
    // broadcast — are only delivered to top-level windows. It is never shown,
    // so the difference is invisible.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        Log::error("could not create the host window: " + Log::lastError());
        return false;
    }

    // Explorer broadcasts this when it restarts. It is the reliable signal that
    // the tray icon needs re-adding and, more importantly here, that the
    // WorkerW every desktop window was parented into has been destroyed.
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

LRESULT CALLBACK AppHost::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppHost* host = nullptr;

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        host = static_cast<AppHost*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        host->window_ = window;
    } else {
        host = reinterpret_cast<AppHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (host != nullptr) return host->handle(message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT AppHost::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        Log::info("Explorer restarted — re-adding the tray icon");
        addTrayIcon();
        engine_.rebuildDesktopWindows();
        updateTrayTooltip();
        return 0;
    }

    switch (message) {
        case WM_TRAY_CALLBACK:
            // Both buttons open the same menu. A tray app with a different
            // left- and right-click menu is a thing users have to learn, and
            // this app has exactly one thing to show.
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP) {
                showMenu();
            }
            return 0;

        case WM_COMMAND:
            onCommand(LOWORD(wParam));
            return 0;

        case WM_IMPORT_FINISHED:
            finishImport();
            return 0;

        case WM_CLOSE:
        case WM_ENDSESSION:
            PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    // Both the power monitor and the engine want some of the same messages —
    // WM_TIMER and WM_SETTINGCHANGE in particular — so both get a look before
    // anything is considered unhandled.
    bool handled = engine_.power().handleMessage(message, wParam, lParam);
    handled = engine_.handleMessage(message, wParam, lParam) || handled;
    if (handled) return 0;

    return DefWindowProcW(window_, message, wParam, lParam);
}

bool AppHost::addTrayIcon() {
    if (trayIcon_ == nullptr) {
        trayIcon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                  MAKEINTRESOURCEW(IDI_TRAYICON), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXSMICON),
                                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (trayIcon_ == nullptr) {
            // No icon resource in this build. A system icon is better than no
            // tray entry at all, which would leave the app with no interface.
            trayIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
        }
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_TRAY_CALLBACK;
    data.hIcon = trayIcon_;
    wcscpy_s(data.szTip, L"LiveWall");

    // Delete first: after an Explorer restart the shell may still hold a record
    // of the old icon, and adding a second one leaves a ghost that does nothing
    // when clicked.
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) return false;

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void AppHost::removeTrayIcon() {
    if (window_ == nullptr) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = static_cast<UINT>(kTrayIconId);
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void AppHost::updateTrayTooltip() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = static_cast<UINT>(kTrayIconId);
    data.uFlags = NIF_TIP | NIF_SHOWTIP;

    // The tooltip is the only always-visible status this app has, so it carries
    // the same line the menu's header does.
    std::string tip = "LiveWall — " + engine_.statusLine();
    if (importing_) tip = format("LiveWall — converting %d%%", importPercent_.load());

    const std::wstring wide = widen(tip);
    wcsncpy_s(data.szTip, wide.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void AppHost::showMenu() {
    TrayMenu::destroy(menu_);
    menu_ = TrayMenu::build(engine_, importing_, importPercent_);
    if (menu_.menu == nullptr) return;

    POINT cursor{};
    GetCursorPos(&cursor);

    // Required, and the reason is not obvious: without it the menu does not
    // dismiss when the user clicks elsewhere, because a tray menu's owner
    // window is not the foreground window and Windows only tracks dismissal for
    // the foreground one.
    SetForegroundWindow(window_);

    TrackPopupMenuEx(menu_.menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, cursor.x,
                     cursor.y, window_, nullptr);

    // The documented companion to the SetForegroundWindow above; it lets the
    // menu close properly on the first click outside it.
    PostMessageW(window_, WM_NULL, 0, 0);
}

void AppHost::onCommand(UINT command) {
    if (command >= IDM_WALLPAPER_FIRST && command <= IDM_WALLPAPER_LAST) {
        const size_t index = command - IDM_WALLPAPER_FIRST;
        if (index < menu_.wallpaperIds.size()) engine_.select(menu_.wallpaperIds[index]);
        return;
    }

    if (command >= IDM_PRESET_FIRST && command <= IDM_PRESET_LAST) {
        const size_t index = command - IDM_PRESET_FIRST;
        const auto presets = Transcoder::allPresets();
        if (index < presets.size()) engine_.library().setPreset(*presets[index]);
        return;
    }

    if (command >= IDM_FIT_FIRST && command <= IDM_FIT_LAST) {
        engine_.setFitMode(static_cast<FitMode>(command - IDM_FIT_FIRST));
        return;
    }

    switch (command) {
        case IDM_PROCEDURAL:
            engine_.select({});
            return;

        case IDM_ADD_VIDEO:
            startImport();
            return;

        case IDM_REMOVE_CURRENT: {
            const std::string id = engine_.library().selectedId();
            if (id.empty()) return;
            engine_.library().remove(id);
            engine_.select({});
            return;
        }

        case IDM_REVEAL_LIBRARY:
            paths::revealInExplorer(engine_.library().directory());
            return;

        case IDM_PAUSE_ON_BATTERY:
            engine_.setPauseOnBattery(!engine_.library().pauseOnBattery());
            return;

        case IDM_START_AT_LOGIN:
            StartupItem::setEnabled(!StartupItem::isEnabled());
            return;

        case IDM_QUIT:
            importCancelled_ = true;
            PostQuitMessage(0);
            return;

        default:
            return;
    }
}

void AppHost::startImport() {
    if (importing_) return;

    wchar_t chosen[MAX_PATH]{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kFileFilter;
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = static_cast<DWORD>(std::size(chosen));
    dialog.lpstrTitle = L"Pick a video — it will be converted to a low-power loop";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    SetForegroundWindow(window_);
    if (GetOpenFileNameW(&dialog) == 0) return;

    const std::wstring source = chosen;
    const std::string id = newGuidString();
    if (id.empty()) {
        showError("Could not generate an identifier for the import.");
        return;
    }

    const std::wstring destination = engine_.library().destinationFor(id);
    // By pointer, and to the static the library returns rather than to a local
    // reference: the worker thread outlives this function, and capturing a
    // reference variable that has gone out of scope is undefined even when the
    // referent is a static that outlives everything.
    const Transcoder::Preset* preset = &engine_.library().preset();
    const Transcoder::DisplayTarget display = Transcoder::DisplayTarget::primary();
    const std::string title = narrow(paths::stem(source));

    importing_ = true;
    importCancelled_ = false;
    importPercent_ = 0;
    importSucceeded_ = false;
    importError_.clear();
    importItemId_ = id;
    updateTrayTooltip();

    if (importThread_.joinable()) importThread_.join();

    // On its own thread because a 4K import takes tens of seconds, and a
    // message loop that stops pumping for that long makes Windows declare the
    // process not responding — at which point the tray icon stops answering
    // clicks and the wallpaper's own timers stop firing.
    importThread_ = std::thread([this, source, destination, preset, display, title, id] {
        Transcoder::Result result;
        const std::string error = Transcoder::convert(
            source, destination, *preset, display,
            [this](double fraction) {
                const int percent = static_cast<int>(fraction * 100);
                if (percent != importPercent_) importPercent_ = percent;
            },
            [this] { return importCancelled_.load(); }, &result);

        if (error.empty()) {
            WallpaperItem item;
            item.id = id;
            item.title = title;
            item.filename = narrow(paths::filename(result.path));
            item.width = result.width;
            item.height = result.height;
            item.fps = result.fps;
            item.byteCount = result.byteCount;
            item.bitDepth = result.bitDepth;
            importedItem_ = std::move(item);
            importSucceeded_ = true;
        } else {
            importError_ = error;
        }

        // Hand the result back to the UI thread. Touching the library or the
        // engine from here would race every menu the user opens.
        PostMessageW(window_, WM_IMPORT_FINISHED, 0, 0);
    });
}

void AppHost::finishImport() {
    if (importThread_.joinable()) importThread_.join();

    importing_ = false;
    importPercent_ = 0;

    if (importSucceeded_) {
        engine_.library().add(importedItem_);
        engine_.select(importedItem_.id);
    } else if (!importError_.empty() && !importCancelled_) {
        showError(importError_);
    }

    importSucceeded_ = false;
    importError_.clear();
    updateTrayTooltip();
}

void AppHost::showError(const std::string& message) {
    SetForegroundWindow(window_);
    MessageBoxW(window_, widen(message).c_str(), L"Couldn't add that video",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

}  // namespace livewall
