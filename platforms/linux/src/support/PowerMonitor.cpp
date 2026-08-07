#include "support/PowerMonitor.h"

#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "support/DBus.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr const char* kLogind = "org.freedesktop.login1";
constexpr const char* kLogindManagerPath = "/org/freedesktop/login1";
constexpr const char* kLogindManager = "org.freedesktop.login1.Manager";
constexpr const char* kLogindSession = "org.freedesktop.login1.Session";

// power-profiles-daemon renamed its bus name in 0.20 and kept the old one as an
// alias for a while. Both are tried; a machine has at most one.
constexpr const char* kPowerProfilesNew = "org.freedesktop.UPower.PowerProfiles";
constexpr const char* kPowerProfilesOld = "net.hadess.PowerProfiles";
constexpr const char* kPowerProfilesPathNew = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char* kPowerProfilesPathOld = "/net/hadess/PowerProfiles";

std::int64_t monotonicMs() {
    timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// A one-line sysfs read. Trimmed, because every one of these files ends in a
// newline that would break every comparison below.
std::string readSysfs(const std::string& path) {
    return std::string(trim(paths::readFile(path)));
}

std::vector<std::string> directoryEntries(const char* path) {
    std::vector<std::string> names;
    DIR* directory = ::opendir(path);
    if (directory == nullptr) return names;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(directory);
    return names;
}

}  // namespace

PowerMonitor::~PowerMonitor() { stop(); }

void PowerMonitor::start() {
    if (started_) return;
    started_ = true;

    if (dbus::Bus* systemBus = dbus::Bus::system(); systemBus != nullptr) {
        haveSystemBus_ = true;
        resolveLogindSession();

        // PrepareForSleep fires before the machine suspends, which is the only
        // notice there is. Without it the decoder is still running when the
        // system goes down and the first frame after resume is decoded against
        // a stale VA-API context.
        systemBus->subscribe(
            "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'");
        if (!logindSessionPath_.empty()) {
            systemBus->subscribe("type='signal',interface='org.freedesktop.DBus.Properties',"
                                 "member='PropertiesChanged',path='" +
                                 logindSessionPath_ + "'");
            haveIdleSignal_ = true;
        }
        systemBus->subscribe("type='signal',interface='org.freedesktop.DBus.Properties',"
                             "member='PropertiesChanged',arg0='" +
                             std::string(kPowerProfilesNew) + "'");
        systemBus->subscribe("type='signal',interface='org.freedesktop.DBus.Properties',"
                             "member='PropertiesChanged',arg0='" +
                             std::string(kPowerProfilesOld) + "'");
    }

    if (dbus::Bus* sessionBus = dbus::Bus::session(); sessionBus != nullptr) {
        haveSessionBus_ = true;
        sessionBus->subscribe(
            "type='signal',interface='org.freedesktop.ScreenSaver',member='ActiveChanged'");
    }

    refreshPowerSupply();
    refreshThermal();
    refreshSessionState();
    lastSlowPollMs_ = monotonicMs();

    Log::info("power gates available: " + capabilities());
}

void PowerMonitor::stop() { started_ = false; }

int PowerMonitor::pollIntervalMs() const { return userIsAway_ ? 1000 : 15000; }

void PowerMonitor::resolveLogindSession() {
    dbus::Bus* bus = dbus::Bus::system();
    if (bus == nullptr) return;

    // GetSessionByPID rather than GetSession($XDG_SESSION_ID): the environment
    // variable is absent under a systemd user unit started before the graphical
    // session, which is exactly how this daemon is normally launched.
    const dbus::Reply reply =
        bus->call(kLogind, kLogindManagerPath, kLogindManager, "GetSessionByPID",
                  {dbus::Value::uint32(static_cast<std::uint32_t>(::getpid()))});

    std::string path;
    if (reply.asString(&path) && !path.empty()) {
        logindSessionPath_ = path;
        Log::info("logind session " + path);
        return;
    }

    // Falling back to "auto" covers the case where the daemon's own pid is not
    // in a session scope — a user unit under systemd --user is in the user
    // slice, not the session scope, on some distributions.
    const dbus::Reply autoReply = bus->call(kLogind, kLogindManagerPath, kLogindManager,
                                            "GetSession", {dbus::Value::string("auto")});
    if (autoReply.asString(&path) && !path.empty()) {
        logindSessionPath_ = path;
        Log::info("logind session " + path + " (auto)");
    } else {
        Log::info("no logind session — the lock and idle-hint gates are off");
    }
}

void PowerMonitor::refreshPowerSupply() {
    haveBattery_ = false;
    bool sawMains = false;
    bool mainsOnline = false;
    double chargeNow = 0;
    double chargeFull = 0;
    std::optional<double> reportedCapacity;

    for (const std::string& name : directoryEntries("/sys/class/power_supply")) {
        const std::string base = std::string("/sys/class/power_supply/") + name;
        const std::string type = readSysfs(base + "/type");

        if (type == "Mains" || type == "USB") {
            sawMains = true;
            if (readSysfs(base + "/online") == "1") mainsOnline = true;
            continue;
        }
        if (type != "Battery") continue;

        // A wireless mouse and a pair of headphones both appear here as
        // batteries. `scope` is "Device" for those and absent or "System" for
        // the machine's own — without this check a flat mouse stops the
        // wallpaper.
        if (const std::string scope = readSysfs(base + "/scope"); scope == "Device") continue;

        haveBattery_ = true;

        // Two ways to express the same thing, and which one a driver publishes
        // is not predictable. `capacity` is already a percentage; the
        // energy/charge pairs need dividing. Summed across packs, because a
        // ThinkPad with two batteries reports each separately.
        if (const std::string capacity = readSysfs(base + "/capacity"); !capacity.empty()) {
            const double value = std::strtod(capacity.c_str(), nullptr);
            reportedCapacity = reportedCapacity.value_or(0) + value;
        }
        for (const char* pair : {"energy", "charge"}) {
            const std::string now = readSysfs(base + "/" + pair + "_now");
            const std::string full = readSysfs(base + "/" + pair + "_full");
            if (now.empty() || full.empty()) continue;
            chargeNow += std::strtod(now.c_str(), nullptr);
            chargeFull += std::strtod(full.c_str(), nullptr);
            break;
        }
    }

    // No mains supply at all means a desktop, which is never "on battery".
    onBattery_ = haveBattery_ && sawMains && !mainsOnline;

    if (chargeFull > 0) {
        batteryFraction_ = chargeNow / chargeFull;
    } else if (reportedCapacity.has_value()) {
        batteryFraction_ = *reportedCapacity / 100.0;
    } else {
        batteryFraction_.reset();
    }
}

void PowerMonitor::refreshThermal() {
    haveThermal_ = false;
    bool hot = false;

    for (const std::string& zone : directoryEntries("/sys/class/thermal")) {
        if (!startsWith(zone, "thermal_zone")) continue;
        const std::string base = std::string("/sys/class/thermal/") + zone;

        const std::string temperature = readSysfs(base + "/temp");
        if (temperature.empty()) continue;
        haveThermal_ = true;

        const double milliCelsius = std::strtod(temperature.c_str(), nullptr);
        // A zone that reads zero or negative is a driver that has nothing to
        // say, not a machine at absolute zero.
        if (milliCelsius <= 0) continue;

        // Each zone publishes its own trip points, and a laptop CPU that idles
        // at 85 C by design would sit permanently over any fixed threshold. The
        // critical trip is the number the kernel itself will act on.
        double threshold = kThermalFallbackMilliCelsius;
        for (int index = 0; index < 16; ++index) {
            const std::string suffix = std::to_string(index);
            const std::string type = readSysfs(base + "/trip_point_" + suffix + "_type");
            if (type.empty()) break;
            if (type != "critical" && type != "hot") continue;
            const std::string trip = readSysfs(base + "/trip_point_" + suffix + "_temp");
            if (trip.empty()) continue;
            const double value = std::strtod(trip.c_str(), nullptr);
            if (value > 0) threshold = std::min(threshold, value - kThermalHeadroomMilliCelsius);
        }

        if (milliCelsius >= threshold) {
            hot = true;
            break;
        }
    }

    thermallyPressured_ = hot;
}

bool PowerMonitor::queryScreenSaver() {
    dbus::Bus* bus = dbus::Bus::session();
    if (bus == nullptr) return false;

    // The freedesktop name is what every desktop implements; the GNOME one is
    // its own and answers when the freedesktop alias is not registered.
    static constexpr struct {
        const char* name;
        const char* path;
        const char* interface;
    } kCandidates[] = {
        {"org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
         "org.freedesktop.ScreenSaver"},
        {"org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver"},
    };

    for (const auto& candidate : kCandidates) {
        const dbus::Reply reply =
            bus->call(candidate.name, candidate.path, candidate.interface, "GetActive");
        bool active = false;
        if (reply.asBool(&active)) {
            haveScreenSaver_ = true;
            return active;
        }
    }
    return false;
}

bool PowerMonitor::queryPowerProfile() {
    dbus::Bus* bus = dbus::Bus::system();
    if (bus == nullptr) return false;

    static constexpr struct {
        const char* name;
        const char* path;
    } kCandidates[] = {
        {kPowerProfilesNew, kPowerProfilesPathNew},
        {kPowerProfilesOld, kPowerProfilesPathOld},
    };

    for (const auto& candidate : kCandidates) {
        const dbus::Reply reply =
            bus->property(candidate.name, candidate.path, candidate.name, "ActiveProfile");
        std::string profile;
        if (reply.asString(&profile)) {
            havePowerProfiles_ = true;
            return profile == "power-saver";
        }
    }
    return false;
}

void PowerMonitor::refreshSessionState() {
    dbus::Bus* bus = dbus::Bus::system();
    if (bus != nullptr && !logindSessionPath_.empty()) {
        bool locked = false;
        if (bus->property(kLogind, logindSessionPath_.c_str(), kLogindSession, "LockedHint")
                .asBool(&locked)) {
            sessionLocked_ = locked;
        }
    }
    screenSaverRunning_ = queryScreenSaver();
    powerSaver_ = queryPowerProfile();
}

void PowerMonitor::poll() {
    const std::int64_t now = monotonicMs();
    // The event loop calls this once per frame. Everything below is either a
    // round trip to the display server or a blocking DBus call, so it runs on
    // its own cadence rather than the frame rate — 1 s while the user looks
    // away, 15 s otherwise, which is what pollIntervalMs() describes.
    if (now < nextPollMs_) return;
    nextPollMs_ = now + pollIntervalMs();

    const bool wasBlocked = systemBlocksRendering();
    const bool wasAway = userIsAway_;

    if (displayAsleepProbe) {
        if (const std::optional<bool> asleep = displayAsleepProbe(); asleep.has_value()) {
            displayOff_ = *asleep;
        }
    }

    // Idle time from the backend where it has it (X11's MIT screen saver
    // extension counts every input device), and from logind's IdleHint where it
    // does not. The hint is coarser — it is whatever the compositor last told
    // logind — but it is the only thing a Wayland client can see.
    std::optional<std::int64_t> idleMs;
    if (idleMillisProbe) idleMs = idleMillisProbe();

    if (idleMs.has_value()) {
        userIsAway_ = *idleMs >= kAwayAfterMs;
    } else if (dbus::Bus* bus = dbus::Bus::system();
               bus != nullptr && !logindSessionPath_.empty()) {
        bool idle = false;
        if (bus->property(kLogind, logindSessionPath_.c_str(), kLogindSession, "IdleHint")
                .asBool(&idle)) {
            userIsAway_ = idle;
        }
    }

    if (now - lastSlowPollMs_ >= 15000) {
        lastSlowPollMs_ = now;
        refreshPowerSupply();
        refreshThermal();
        // The screen saver and the power profile both emit signals, so these
        // reads are a backstop for a desktop that emits neither rather than the
        // primary path.
        screenSaverRunning_ = queryScreenSaver();
        powerSaver_ = queryPowerProfile();
    }

    if (userIsAway_ != wasAway) {
        Log::info(userIsAway_ ? "user away — stopping" : "user back — resuming");
        // The interval depends on which side of this we are on: noticing the
        // user came back has to be quick, or the wallpaper visibly sits frozen
        // after the first keypress.
        nextPollMs_ = monotonicMs() + pollIntervalMs();
    }
    if (systemBlocksRendering() != wasBlocked && onChange) onChange();
}

void PowerMonitor::handleBusSignal(std::string_view interface, std::string_view member,
                                   std::string_view path) {
    const bool wasBlocked = systemBlocksRendering();

    if (interface == "org.freedesktop.login1.Manager" && member == "PrepareForSleep") {
        // The signal's boolean argument says whether the machine is going down
        // or coming back, and the wrapper does not surface signal arguments.
        // Toggling is correct because the two always alternate, and the
        // recovery if one is missed is the next poll: `suspending_` only gates
        // rendering, and every other gate still applies.
        suspending_ = !suspending_;
        Log::info(suspending_ ? "system suspending" : "system resumed");
    } else if (interface == "org.freedesktop.DBus.Properties" && member == "PropertiesChanged") {
        if (!logindSessionPath_.empty() && path == logindSessionPath_) {
            refreshSessionState();
        } else {
            powerSaver_ = queryPowerProfile();
        }
    } else if (interface == "org.freedesktop.ScreenSaver" && member == "ActiveChanged") {
        screenSaverRunning_ = queryScreenSaver();
    } else {
        return;
    }

    if (systemBlocksRendering() != wasBlocked && onChange) onChange();
}

void PowerMonitor::setPauseOnBattery(bool value) {
    if (pauseOnBattery_ == value) return;
    pauseOnBattery_ = value;
    if (onChange) onChange();
}

std::optional<std::string> PowerMonitor::blockReason() const {
    if (suspending_) return "system suspending";
    if (displayOff_) return "display asleep";
    if (sessionLocked_) return "screen locked";
    if (screenSaverRunning_) return "screensaver";
    if (userIsAway_) return "no one here";
    if (thermallyPressured_) return "machine running hot";
    if (powerSaver_) return "power-saver profile";
    if (onBattery_) {
        if (pauseOnBattery_) return "on battery";
        if (batteryFraction_.has_value() && *batteryFraction_ < kLowBatteryFraction) {
            return "battery low";
        }
    }
    return std::nullopt;
}

std::string PowerMonitor::capabilities() const {
    std::vector<std::string> missing;
    if (!haveSystemBus_) missing.emplace_back("system bus (lock, idle hint, suspend, power profile)");
    if (!haveSessionBus_) missing.emplace_back("session bus (screensaver)");
    if (haveSystemBus_ && logindSessionPath_.empty()) missing.emplace_back("logind session (lock)");
    if (haveSessionBus_ && !haveScreenSaver_) missing.emplace_back("screensaver service");
    if (haveSystemBus_ && !havePowerProfiles_) missing.emplace_back("power-profiles-daemon");
    if (!haveThermal_) missing.emplace_back("thermal zones");
    if (!haveBattery_) missing.emplace_back("battery (desktop machine)");
    if (!displayAsleepProbe) missing.emplace_back("display sleep (backend cannot report it)");
    if (!idleMillisProbe && !haveIdleSignal_) missing.emplace_back("idle time");

    if (missing.empty()) return "all";

    std::string out = "missing — ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) out += "; ";
        out += missing[i];
    }
    return out;
}

}  // namespace livewall
