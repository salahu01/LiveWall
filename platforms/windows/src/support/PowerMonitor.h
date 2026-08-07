// Watches every machine-wide condition that should stop wallpaper rendering.
//
// Per-monitor coverage is handled by `MonitorController`; this covers the rest:
// session lock, display off, screen saver, battery saver, a nearly flat
// battery, an absent user, and (optionally) running on battery at all.
//
// Every one of these is a hard stop rather than a slowdown, and that is forced
// by the decode path exactly as it is on macOS: playback pulls one frame per
// tick and the assets have no B-frames, so every frame is a reference frame
// that must be decoded whether or not it is shown. Lowering the tick rate would
// play the clip in slow motion, not more cheaply. Stopping is free *and*
// graceful — the last presented frame stays in the swap chain's front buffer,
// so a stopped wallpaper looks like a still rather than a black rectangle.
//
// One Windows-specific absence, stated rather than hidden: there is no
// equivalent of `ProcessInfo.thermalState`. Windows exposes thermal information
// only through vendor drivers and WMI classes that are not present on every
// machine, and polling WMI would cost more than the wallpaper does. Battery
// saver is the closest proxy the OS actually guarantees, and it is what the
// gate below uses.
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace livewall {

class PowerMonitor {
public:
    PowerMonitor() = default;
    ~PowerMonitor();

    PowerMonitor(const PowerMonitor&) = delete;
    PowerMonitor& operator=(const PowerMonitor&) = delete;

    // Called whenever the answer to "should we render at all?" may have
    // changed. Always on the thread that owns `window`.
    std::function<void()> onChange;

    // `window` receives the notifications. It must be the app's message-only
    // window, and it must forward WM_POWERBROADCAST, WM_WTSSESSION_CHANGE,
    // WM_SETTINGCHANGE and WM_TIMER to `handleMessage`.
    void start(HWND window);
    void stop();

    // Returns true when the message was one of ours.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // User setting: suspend rendering whenever the machine is on battery.
    void setPauseOnBattery(bool value);
    bool pauseOnBattery() const { return pauseOnBattery_; }

    // True when nothing on this machine should be rendering, whatever any
    // individual display's coverage says.
    bool systemBlocksRendering() const { return blockReason().has_value(); }

    // Human-readable reason rendering is suspended, for the tray menu. This is
    // also the definition of `systemBlocksRendering` — one list, so the menu
    // can never claim a reason the gate does not actually apply.
    std::optional<std::string> blockReason() const;

    bool isOnBattery() const { return onBattery_; }
    // Remaining charge as 0...1, or nothing on a machine with no battery.
    std::optional<double> batteryFraction() const { return batteryFraction_; }

private:
    void refreshPowerSource();
    void pollPolledSignals();
    void rescheduleHeartbeat();
    void registerPowerNotifications();
    void unregisterPowerNotifications();

    // No input for this long means nobody is looking at the desktop.
    //
    // Deliberately longer than it needs to be to catch a real absence: the
    // display timeout and the screen saver already handle those, so the only
    // thing this adds is the case where both are disabled. Five minutes was
    // tried on the macOS side and it froze the wallpaper while the user was
    // sitting there reading, which reads as a bug rather than a saving.
    static constexpr DWORD kAwayAfterMs = 15 * 60 * 1000;

    // Below this, on battery, the wallpaper stops regardless of the
    // pause-on-battery setting. Ambient decoration is not what the last of a
    // charge is for.
    static constexpr double kLowBatteryFraction = 0.20;

    static constexpr UINT_PTR kHeartbeatTimerId = 1;

    HWND window_ = nullptr;

    HPOWERNOTIFY displayStateNotify_ = nullptr;
    HPOWERNOTIFY powerSavingNotify_ = nullptr;
    HPOWERNOTIFY acdcNotify_ = nullptr;
    bool sessionNotificationsRegistered_ = false;

    bool displayOff_ = false;
    bool sessionLocked_ = false;
    bool screenSaverRunning_ = false;
    bool userIsAway_ = false;
    bool batterySaver_ = false;
    bool onBattery_ = false;
    bool pauseOnBattery_ = false;
    std::optional<double> batteryFraction_;
};

}  // namespace livewall
