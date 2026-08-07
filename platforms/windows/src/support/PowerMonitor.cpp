#include "support/PowerMonitor.h"

#include <wtsapi32.h>

#include "support/DesktopVisibility.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// The power-setting GUIDs, defined here rather than taken from the SDK.
//
// winnt.h declares them with DEFINE_GUID, which without INITGUID produces an
// `extern const GUID` whose definition lives in a library that varies by SDK
// and target — some configurations resolve it from PowrProf.lib, some from
// mincore.lib, and some not at all, which surfaces as an unresolved external
// rather than as anything that names the cause. Three sixteen-byte constants
// are cheaper than that uncertainty, and their values are documented and fixed.
constexpr GUID kSessionDisplayStatus = {
    0x2B84C20E, 0xAD23, 0x4DDF, {0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}};
constexpr GUID kPowerSavingStatus = {
    0xE00958C0, 0xC213, 0x4ACE, {0xAC, 0x77, 0xFE, 0xCC, 0xED, 0x2E, 0xEE, 0xA5}};
constexpr GUID kAcDcPowerSource = {
    0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};

// Milliseconds since the user last touched anything. GetLastInputInfo covers
// keyboard and mouse without an input hook, so it needs none of the permissions
// or the message-pump cost a hook would.
DWORD idleMilliseconds() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (GetLastInputInfo(&info) == 0) return 0;

    const DWORD now = GetTickCount();
    // GetTickCount wraps every 49.7 days and dwTime is on the same clock, so
    // unsigned subtraction is correct across the wrap and a signed compare
    // would not be.
    return now - info.dwTime;
}

bool screenSaverActive() {
    BOOL running = FALSE;
    if (SystemParametersInfoW(SPI_GETSCREENSAVERRUNNING, 0, &running, 0) == 0) return false;
    return running != FALSE;
}

}  // namespace

PowerMonitor::~PowerMonitor() { stop(); }

void PowerMonitor::start(HWND window) {
    window_ = window;
    registerPowerNotifications();

    // Session lock and unlock. Without WTSRegisterSessionNotification the only
    // signal is the display turning off, and a user who locks and walks away
    // with the display timeout set to an hour would keep decoding for that hour.
    if (WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != 0) {
        sessionNotificationsRegistered_ = true;
    } else {
        Log::error("session lock notifications unavailable: " + Log::lastError());
    }

    refreshPowerSource();
    screenSaverRunning_ = screenSaverActive();
    rescheduleHeartbeat();
}

void PowerMonitor::stop() {
    if (window_ == nullptr) return;

    KillTimer(window_, kHeartbeatTimerId);
    if (sessionNotificationsRegistered_) {
        WTSUnRegisterSessionNotification(window_);
        sessionNotificationsRegistered_ = false;
    }
    unregisterPowerNotifications();
    window_ = nullptr;
}

void PowerMonitor::registerPowerNotifications() {
    // GUID_SESSION_DISPLAY_STATUS rather than GUID_CONSOLE_DISPLAY_STATE: the
    // console variant is documented as reporting the state of the *console*
    // display and is not delivered to a session that is not the console one.
    // The session variant is the one that fires for the display this app's
    // windows are actually on.
    displayStateNotify_ = RegisterPowerSettingNotification(
        window_, &kSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerSavingNotify_ = RegisterPowerSettingNotification(
        window_, &kPowerSavingStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    acdcNotify_ = RegisterPowerSettingNotification(
        window_, &kAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void PowerMonitor::unregisterPowerNotifications() {
    if (displayStateNotify_ != nullptr) UnregisterPowerSettingNotification(displayStateNotify_);
    if (powerSavingNotify_ != nullptr) UnregisterPowerSettingNotification(powerSavingNotify_);
    if (acdcNotify_ != nullptr) UnregisterPowerSettingNotification(acdcNotify_);
    displayStateNotify_ = powerSavingNotify_ = acdcNotify_ = nullptr;
}

bool PowerMonitor::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
                refreshPowerSource();
                if (onChange) onChange();
                return true;
            }
            if (wParam != PBT_POWERSETTINGCHANGE) return false;

            const auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
            if (setting == nullptr) return false;
            const DWORD value = setting->Data[0];

            if (IsEqualGUID(setting->PowerSetting, kSessionDisplayStatus)) {
                // 0 = off, 1 = on, 2 = dimmed. Dimmed still shows the desktop,
                // so only a hard off counts.
                const bool off = (value == 0);
                if (off != displayOff_) {
                    displayOff_ = off;
                    Log::info(off ? "display off — stopping" : "display on — resuming");
                    if (onChange) onChange();
                }
                return true;
            }
            if (IsEqualGUID(setting->PowerSetting, kPowerSavingStatus)) {
                batterySaver_ = (value != 0);
                if (onChange) onChange();
                return true;
            }
            if (IsEqualGUID(setting->PowerSetting, kAcDcPowerSource)) {
                refreshPowerSource();
                if (onChange) onChange();
                return true;
            }
            return false;
        }

        case WM_WTSSESSION_CHANGE: {
            if (wParam == WTS_SESSION_LOCK) {
                sessionLocked_ = true;
            } else if (wParam == WTS_SESSION_UNLOCK) {
                sessionLocked_ = false;
            } else if (wParam == WTS_SESSION_LOGOFF || wParam == WTS_CONSOLE_DISCONNECT) {
                sessionLocked_ = true;
            } else if (wParam == WTS_CONSOLE_CONNECT) {
                sessionLocked_ = false;
            } else {
                return false;
            }
            Log::info(sessionLocked_ ? "session locked — stopping" : "session unlocked — resuming");
            if (onChange) onChange();
            return true;
        }

        case WM_TIMER:
            if (wParam == kHeartbeatTimerId) {
                pollPolledSignals();
                return true;
            }
            return false;

        case WM_SETTINGCHANGE:
            // Covers SPI_SETSCREENSAVERRUNNING among others; the poll below
            // reads the actual state rather than trusting the parameter.
            pollPolledSignals();
            return true;

        default:
            return false;
    }
}

void PowerMonitor::setPauseOnBattery(bool value) {
    if (pauseOnBattery_ == value) return;
    pauseOnBattery_ = value;
    if (onChange) onChange();
}

void PowerMonitor::refreshPowerSource() {
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) == 0) {
        onBattery_ = false;
        batteryFraction_.reset();
        return;
    }

    // ACLineStatus: 0 offline, 1 online, 255 unknown. Unknown is treated as
    // mains, because the failure mode of guessing "battery" is a desktop PC
    // whose wallpaper stops for a battery it does not have.
    onBattery_ = (status.ACLineStatus == 0);

    if ((status.BatteryFlag & BATTERY_FLAG_NO_BATTERY) != 0 ||
        status.BatteryLifePercent == 255) {
        batteryFraction_.reset();
    } else {
        batteryFraction_ = status.BatteryLifePercent / 100.0;
    }

    // Battery saver is also reported here, which covers the case where the app
    // started while it was already on and no notification ever fires.
    batterySaver_ = (status.SystemStatusFlag != 0);
}

void PowerMonitor::rescheduleHeartbeat() {
    if (window_ == nullptr) return;

    // The interval is asymmetric on purpose. Noticing that the user *left* can
    // be lazy — nothing is wrong with rendering fifteen seconds longer than
    // strictly needed. Noticing they came *back* has to be quick, or the
    // wallpaper visibly sits frozen after the first keypress.
    const UINT interval = userIsAway_ ? 1000 : 15000;
    SetTimer(window_, kHeartbeatTimerId, interval, nullptr);
}

void PowerMonitor::pollPolledSignals() {
    bool changed = false;

    const bool wasOnBattery = onBattery_;
    const bool wasSaver = batterySaver_;
    refreshPowerSource();
    if (wasOnBattery != onBattery_ || wasSaver != batterySaver_) changed = true;

    const bool saver = screenSaverActive();
    if (saver != screenSaverRunning_) {
        screenSaverRunning_ = saver;
        changed = true;
    }

    const bool away = idleMilliseconds() >= kAwayAfterMs;
    if (away != userIsAway_) {
        userIsAway_ = away;
        Log::info(away ? "user away — stopping" : "user back — resuming");
        // The polling rate depends on which side of this we are on.
        rescheduleHeartbeat();
        changed = true;
    }

    if (changed && onChange) onChange();
}

std::optional<std::string> PowerMonitor::blockReason() const {
    if (displayOff_) return "display off";
    if (sessionLocked_) return "session locked";
    if (screenSaverRunning_) return "screen saver";
    if (userIsAway_) return "no one here";
    // A full-screen game is the Windows equivalent of "the desktop is covered",
    // but it belongs here rather than in the per-monitor coverage gate: an
    // exclusive-mode swapchain may not appear in the window list at all, so the
    // grid would report the desktop as wide open behind it.
    if (DesktopVisibility::fullScreenAppRunning()) return "a full-screen app is running";
    if (batterySaver_) return "Battery Saver";
    if (onBattery_) {
        if (pauseOnBattery_) return "on battery";
        if (batteryFraction_.has_value() && *batteryFraction_ < kLowBatteryFraction) {
            return "battery low";
        }
    }
    return std::nullopt;
}

}  // namespace livewall
