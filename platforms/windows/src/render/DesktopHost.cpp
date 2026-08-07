#include "render/DesktopHost.h"

#include <iterator>
#include <string>

#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr wchar_t kWindowClass[] = L"LiveWallDesktopWindow";

// Undocumented, and the entire reason this file exists. Progman responds to it
// by spawning (or revealing) a WorkerW window that owns the wallpaper, leaving
// SHELLDLL_DefView above it. The two parameter shapes below are both in the
// wild: the second is what current Windows 10 and 11 builds want, the first is
// what older ones responded to, and sending both is harmless because Progman
// ignores the one it does not understand.
constexpr UINT WM_SPAWN_WORKERW = 0x052C;

bool g_usedFallback = false;
HWND g_cachedParent = nullptr;

std::wstring classNameOf(HWND window) {
    wchar_t name[64]{};
    const int length = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return std::wstring(name, length > 0 ? static_cast<size_t>(length) : 0);
}

struct FindContext {
    HWND workerW = nullptr;
};

BOOL CALLBACK findWorkerW(HWND top, LPARAM parameter) {
    auto* context = reinterpret_cast<FindContext*>(parameter);

    // The WorkerW we want is the sibling that follows the one hosting
    // SHELLDLL_DefView. Identifying it by "the one with a DefView child" and
    // then stepping to the next window in Z order is the arrangement Explorer
    // actually produces; searching for "a WorkerW with no children" finds the
    // right window on some builds and a decoy on others.
    const HWND defView = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView == nullptr) return TRUE;

    const HWND sibling = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
    if (sibling != nullptr) {
        context->workerW = sibling;
        return FALSE;  // found it; stop enumerating
    }
    return TRUE;
}

// Explorer is asked to produce the arrangement, then it is verified. It does
// not always happen on the first message — the shell may be mid-initialisation
// at login, which is exactly when a start-at-login copy runs.
HWND spawnAndFindWorkerW() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman == nullptr) {
        Log::error("Progman is not running — Explorer may not be the shell");
        return nullptr;
    }

    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, WM_SPAWN_WORKERW, 0, 0, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, WM_SPAWN_WORKERW, 0x0D, 0x01, SMTO_NORMAL, 1000, &result);

    FindContext context;
    EnumWindows(&findWorkerW, reinterpret_cast<LPARAM>(&context));
    if (context.workerW != nullptr) return context.workerW;

    // Some configurations — a few multi-monitor setups, and Windows 11 with
    // certain shell extensions — keep SHELLDLL_DefView under Progman itself
    // rather than under a WorkerW. There the first free WorkerW is the target.
    HWND candidate = nullptr;
    while ((candidate = FindWindowExW(nullptr, candidate, L"WorkerW", nullptr)) != nullptr) {
        if (FindWindowExW(candidate, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr &&
            IsWindowVisible(candidate) != 0) {
            return candidate;
        }
    }
    return nullptr;
}

LRESULT CALLBACK desktopWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // Never take focus, never take clicks. Returning HTTRANSPARENT alone is
        // not enough — WS_EX_TRANSPARENT on the window is what makes the hit
        // test skip us entirely — but both together mean a click lands on the
        // desktop underneath and selects icons as usual.
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        // The window paints nothing itself; DirectComposition owns its content.
        // Answering WM_ERASEBKGND with 1 and WM_PAINT with an empty
        // begin/end pair stops the DWM asking again.
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool ensureWindowClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &desktopWindowProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kWindowClass;
        // No background brush: anything Windows paints here would cover the
        // system wallpaper before the first frame arrives. The macOS version
        // makes the same choice for the same reason — "no wallpaper of ours"
        // must degrade to the system's own picture, not to a black rectangle.
        description.hbrBackground = nullptr;
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

}  // namespace

HWND DesktopHost::resolveWallpaperParent() {
    if (g_cachedParent != nullptr && IsWindow(g_cachedParent) != 0) return g_cachedParent;

    g_usedFallback = false;
    HWND parent = spawnAndFindWorkerW();

    if (parent == nullptr) {
        // Parenting into Progman still gets a window that lives with the
        // desktop, is excluded from Alt-Tab and survives Show Desktop. What it
        // loses is the ordering against the icons: the wallpaper draws over
        // them. Worth having as a fallback, worth telling the user about.
        parent = FindWindowW(L"Progman", nullptr);
        g_usedFallback = (parent != nullptr);
        if (g_usedFallback) {
            Log::error("could not find the WorkerW behind the desktop icons; "
                       "falling back to Progman — the wallpaper will cover desktop icons");
        }
    }

    g_cachedParent = parent;
    return parent;
}

bool DesktopHost::usingFallbackParent() { return g_usedFallback; }

void DesktopHost::invalidateParent() {
    g_cachedParent = nullptr;
    g_usedFallback = false;
}

std::unique_ptr<DesktopHost> DesktopHost::create(HMONITOR monitor) {
    std::unique_ptr<DesktopHost> host(new DesktopHost());
    if (!host->initialise(monitor)) return nullptr;
    return host;
}

DesktopHost::~DesktopHost() {
    // Order matters: the composition target holds a reference to the HWND, and
    // destroying the window first leaves the DWM briefly composing a target
    // whose window is gone.
    visual_.Reset();
    target_.Reset();
    compositionDevice_.Reset();
    if (window_ != nullptr) DestroyWindow(window_);
}

bool DesktopHost::initialise(HMONITOR monitor) {
    if (!ensureWindowClass()) {
        Log::error("could not register the desktop window class: " + Log::lastError());
        return false;
    }

    monitor_ = monitor;
    parent_ = resolveWallpaperParent();
    if (parent_ == nullptr) {
        Log::error("no wallpaper parent window is available");
        return false;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor_, &info) == 0) return false;

    // The parent covers the whole virtual desktop, and its client origin is the
    // virtual screen's top-left — which on a multi-monitor setup with a
    // secondary display to the left is a negative coordinate. Child positions
    // are therefore monitor coordinates minus that origin.
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    width_ = info.rcMonitor.right - info.rcMonitor.left;
    height_ = info.rcMonitor.bottom - info.rcMonitor.top;

    window_ = CreateWindowExW(
        // NOREDIRECTIONBITMAP: no redirection surface, because DirectComposition
        // supplies the content. Without it the window also owns a full-screen
        // BGRA bitmap it never uses — 24 MB on a 4K panel, for nothing.
        WS_EX_NOREDIRECTIONBITMAP |
            WS_EX_TRANSPARENT |   // clicks pass through to the desktop
            WS_EX_NOACTIVATE |    // never becomes the foreground window
            WS_EX_TOOLWINDOW,     // never appears in Alt-Tab or the taskbar
        kWindowClass, L"LiveWall",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        info.rcMonitor.left - originX, info.rcMonitor.top - originY, width_, height_,
        parent_, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (window_ == nullptr) {
        Log::error("could not create the desktop window: " + Log::lastError());
        return false;
    }

    if (!createCompositionTree()) {
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    Log::info(format("desktop window on monitor %p: %dx%d at (%d, %d)",
                     static_cast<void*>(monitor_), width_, height_,
                     info.rcMonitor.left, info.rcMonitor.top));
    return true;
}

bool DesktopHost::createCompositionTree() {
    // One composition device per window rather than one shared: an
    // IDCompositionTarget is bound to a single HWND, and the device that owns
    // it costs a few kilobytes. Null for the DXGI device means "the default
    // one" — the swap chain the caller attaches carries its own device, and
    // composition does not require the two to match.
    HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&compositionDevice_));
    if (FAILED(hr)) {
        Log::error("DirectComposition is unavailable: " + Log::hresult(hr));
        return false;
    }

    // TRUE for topmost within the target — this window has exactly one visual,
    // so the flag only affects our own tree.
    hr = compositionDevice_->CreateTargetForHwnd(window_, TRUE, &target_);
    if (FAILED(hr)) {
        Log::error("could not create a composition target for the desktop window: " +
                   Log::hresult(hr));
        return false;
    }

    hr = compositionDevice_->CreateVisual(&visual_);
    if (FAILED(hr)) return false;

    hr = target_->SetRoot(visual_.Get());
    if (FAILED(hr)) return false;

    return SUCCEEDED(compositionDevice_->Commit());
}

bool DesktopHost::updateGeometry() {
    if (window_ == nullptr) return false;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor_, &info) == 0) return false;

    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;

    const bool resized = (width != width_ || height != height_);
    width_ = width;
    height_ = height;

    SetWindowPos(window_, nullptr, info.rcMonitor.left - originX, info.rcMonitor.top - originY,
                 width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);

    // Only a size change needs the swap chain rebuilt; a pure move does not,
    // and rebuilding on every WM_DISPLAYCHANGE was what made the macOS version
    // churn decoders several times a minute before it learned to tell the two
    // apart.
    return resized;
}

bool DesktopHost::isOrphaned() const {
    if (window_ == nullptr) return true;
    if (IsWindow(window_) == 0) return true;
    return parent_ == nullptr || IsWindow(parent_) == 0;
}

void DesktopHost::commit() {
    if (compositionDevice_) compositionDevice_->Commit();
}

}  // namespace livewall
