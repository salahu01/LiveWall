// Watches every machine-wide condition that should stop wallpaper rendering.
//
// Per-output coverage is handled by `OutputController`; this covers the rest:
// session lock, display off, screen saver, a power-saving profile, thermal
// pressure, a nearly flat battery, an absent user, an imminent suspend, and
// (optionally) running on battery at all.
//
// Every one of these is a hard stop rather than a slowdown, for the same reason
// as on the other two platforms: playback pulls one frame per tick and the
// assets have no B-frames, so every frame is a reference frame that must be
// decoded whether or not it is shown. Lowering the tick rate would play the
// clip in slow motion, not more cheaply. Stopping is free *and* graceful — the
// last frame stays in the surface the compositor is already showing, so a
// stopped wallpaper looks like a still rather than a black rectangle.
//
// Where the signals come from, and why not from somewhere more obvious:
//
//   Battery, charge, thermal   /sys, read directly. UPower would answer the
//                              first two and is not installed everywhere;
//                              /sys/class/power_supply is in the kernel. And
//                              Linux is the only one of the three platforms
//                              that exposes thermal state at all to an
//                              unprivileged process — Windows has nothing
//                              equivalent and the port there says so.
//
//   Lock, idle hint, suspend   logind on the system bus. The one piece of
//                              session state with a single answer across
//                              desktops.
//
//   Screen saver               org.freedesktop.ScreenSaver on the session bus,
//                              which GNOME, KDE, XFCE and Cinnamon all
//                              implement even when they disagree about
//                              everything else.
//
//   Power-saving profile       power-profiles-daemon. The closest thing Linux
//                              has to Low Power Mode.
//
//   Display asleep, idle time  Neither has a portable answer, so the backend
//                              supplies them: X11 has DPMS and the MIT screen
//                              saver extension, Wayland has neither and falls
//                              back to logind's IdleHint.
//
// Everything above is optional. A machine with no DBus keeps the /sys gates and
// loses the rest; `capabilities()` says which, and `livewall status` prints it,
// because a gate that silently does not exist is worse than one that is
// documented as missing.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace livewall {

class PowerMonitor {
public:
    PowerMonitor() = default;
    ~PowerMonitor();

    PowerMonitor(const PowerMonitor&) = delete;
    PowerMonitor& operator=(const PowerMonitor&) = delete;

    // Called whenever the answer to "should we render at all?" may have
    // changed. Always on the event loop's thread.
    std::function<void()> onChange;

    // Supplied by the display backend before start(). Returning nothing means
    // "this backend cannot tell", which is different from "no".
    std::function<std::optional<bool>()> displayAsleepProbe;
    std::function<std::optional<std::int64_t>()> idleMillisProbe;

    void start();
    void stop();

    // Driven by the event loop rather than by a timer this class owns, so the
    // app has exactly one thing that decides when to wake up.
    //
    // Called on every pass of that loop — which is once per rendered frame, so
    // ten times a second at the default procedural rate. It therefore has to
    // rate-limit itself: the signals it reads are a DPMS round trip to the X
    // server and, on Wayland, a blocking DBus property read from logind.
    // Doing either of those per frame would cost more than the wallpaper.
    void poll();

    // How long the loop may sleep before calling poll() again.
    //
    // Asymmetric on purpose. Noticing that the user *left* can be lazy —
    // nothing is wrong with rendering fifteen seconds longer than strictly
    // needed. Noticing they came *back* has to be quick, or the wallpaper
    // visibly sits frozen after the first keypress.
    int pollIntervalMs() const;

    // Routed in by AppHost from the bus it is already polling.
    void handleBusSignal(std::string_view interface, std::string_view member,
                         std::string_view path);

    // User setting: suspend rendering whenever the machine is on battery.
    void setPauseOnBattery(bool value);
    bool pauseOnBattery() const { return pauseOnBattery_; }

    // True when nothing on this machine should be rendering, whatever any
    // individual output's coverage says.
    bool systemBlocksRendering() const { return blockReason().has_value(); }

    // Human-readable reason rendering is suspended. This is also the definition
    // of `systemBlocksRendering` — one list, so the status line can never claim
    // a reason the gate does not actually apply.
    std::optional<std::string> blockReason() const;

    bool isOnBattery() const { return onBattery_; }
    // Remaining charge as 0...1, or nothing on a machine with no battery.
    std::optional<double> batteryFraction() const { return batteryFraction_; }

    // One line per gate the app could not wire up, for `livewall status`.
    std::string capabilities() const;

private:
    void refreshPowerSupply();
    void refreshThermal();
    void refreshSessionState();
    void resolveLogindSession();
    bool queryScreenSaver();
    bool queryPowerProfile();

    // No input for this long means nobody is looking at the desktop.
    //
    // Deliberately longer than it needs to be to catch a real absence: the
    // display timeout and the screen saver already handle those, so the only
    // thing this adds is the case where both are disabled. Five minutes was
    // tried on the macOS side and it froze the wallpaper while the user was
    // sitting there reading, which reads as a bug rather than a saving.
    static constexpr std::int64_t kAwayAfterMs = 15 * 60 * 1000;

    // Below this, on battery, the wallpaper stops regardless of the
    // pause-on-battery setting. Ambient decoration is not what the last of a
    // charge is for.
    static constexpr double kLowBatteryFraction = 0.20;

    // Hot enough that the machine is about to throttle itself. Taken from each
    // zone's own critical trip point where the kernel publishes one, so a
    // laptop that runs at 85 C by design is not permanently gated.
    static constexpr double kThermalHeadroomMilliCelsius = 5000;
    static constexpr double kThermalFallbackMilliCelsius = 90000;

    bool started_ = false;

    bool displayOff_ = false;
    bool sessionLocked_ = false;
    bool screenSaverRunning_ = false;
    bool userIsAway_ = false;
    bool powerSaver_ = false;
    bool thermallyPressured_ = false;
    bool suspending_ = false;
    bool onBattery_ = false;
    bool pauseOnBattery_ = false;
    std::optional<double> batteryFraction_;

    // Empty when logind is unreachable, which is also how the lock and
    // idle-hint gates are known to be unavailable.
    std::string logindSessionPath_;

    bool haveSystemBus_ = false;
    bool haveSessionBus_ = false;
    bool haveScreenSaver_ = false;
    bool havePowerProfiles_ = false;
    bool haveThermal_ = false;
    bool haveBattery_ = false;
    bool haveIdleSignal_ = false;

    // Rechecked less often than the fast signals: a thermal zone that has just
    // crossed its trip point will still be over it a few seconds later, and
    // reading a dozen sysfs files belongs nowhere near a one-second tick.
    std::int64_t lastSlowPollMs_ = 0;

    // When the fast signals — display sleep and idle time — may next be read.
    // See poll().
    std::int64_t nextPollMs_ = 0;
};

}  // namespace livewall
