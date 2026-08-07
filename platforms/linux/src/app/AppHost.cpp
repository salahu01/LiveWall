#include "app/AppHost.h"

#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>

#include "import/CodecSupport.h"
#include "import/FFmpeg.h"
#include "import/Transcoder.h"
#include "support/DBus.h"
#include "support/Footprint.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/StartupItem.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// Every descriptor the loop watches. Fixed-size because the set never changes
// after startup — a bus that was absent then stays absent.
enum Watched { kBackendFd, kControlFd, kSessionBusFd, kSystemBusFd, kSignalFd, kWatchedCount };

}  // namespace

AppHost::AppHost() = default;

AppHost::~AppHost() {
    if (signalFd_ >= 0) ::close(signalFd_);
}

bool AppHost::setUp() {
    StartupItem::reconcile();

    if (ControlSocket::daemonRunning()) {
        Log::error("LiveWall is already running.");
        return false;
    }

    // The settings are read before the engine exists because the backend choice
    // lives in them and the backend has to be built first.
    Settings settings;
    const std::string preference = settings.stringValue(Settings::kBackend, "auto");

    backend_ = Backend::create(preference);
    if (!backend_) return false;

    // Alpha is only meaningful when something composites it. On Wayland that is
    // always true — the compositor *is* the compositor. On X11 it depends on
    // whether a compositing manager is running, and asking for an ARGB visual
    // without one produces opaque black where the Fit letterbox should show the
    // desktop underneath.
    const bool wantAlpha = backend_->prefersAlpha();
    if (!wantAlpha) {
        Log::info("no compositing manager — Fit mode's letterbox will be black, not the "
                  "desktop behind");
    }

    egl_ = EglDevice::create(*backend_, wantAlpha);
    if (!egl_) return false;

    // The app paces itself at the wallpaper's own frame rate, a quarter or a
    // fifth of the refresh. A swap interval of 1 would block this single thread
    // inside eglSwapBuffers until the next vblank, stalling the control socket,
    // both buses and every other output behind it.
    egl_->setSwapInterval(0);

    engine_ = std::make_unique<WallpaperEngine>(*backend_, *egl_);

    if (!control_.listen([this](const std::string& command,
                                const std::vector<std::string>& arguments) {
            return handleCommand(command, arguments);
        })) {
        Log::error("could not take the control socket — is another copy starting?");
        return false;
    }

    engine_->onStateChange = [this]() { tray_.refresh(); };
    engine_->start();

    tray_.start(*engine_);

    for (dbus::Bus* bus : {dbus::Bus::session(), dbus::Bus::system()}) {
        if (bus == nullptr) continue;
        bus->setSignalHandler([this](std::string_view interface, std::string_view member,
                                     std::string_view path) {
            onBusSignal(interface, member, path);
        });
    }

    // Blocked here and read through signalfd rather than handled in a signal
    // handler. A handler can only safely set a flag, and the loop is asleep in
    // poll() — which would then need the self-pipe this replaces.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signalFd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    // Reaps xdg-open and anything else spawned and forgotten, without a
    // handler. Nothing here ever waits on a child's status.
    ::signal(SIGCHLD, SIG_IGN);

    Log::info("started on " + std::string(backend_->name()) + ", " + Footprint::formatted());
    return true;
}

void AppHost::onBusSignal(std::string_view interface, std::string_view member,
                          std::string_view path) {
    engine_->power().handleBusSignal(interface, member, path);
    tray_.handleBusSignal(interface, member);
}

void AppHost::loop() {
    std::array<pollfd, kWatchedCount> descriptors = {};

    descriptors[kBackendFd].fd = backend_->eventFd();
    descriptors[kControlFd].fd = control_.fd();
    descriptors[kSessionBusFd].fd = dbus::Bus::session() != nullptr ? dbus::Bus::session()->fd() : -1;
    descriptors[kSystemBusFd].fd = dbus::Bus::system() != nullptr ? dbus::Bus::system()->fd() : -1;
    descriptors[kSignalFd].fd = signalFd_;
    for (pollfd& descriptor : descriptors) descriptor.events = POLLIN;

    while (running_) {
        const int sleepMs = engine_->tick();

        // Anything queued for the display server has to reach it before this
        // thread goes to sleep, or a Wayland commit sits in the buffer until
        // the next unrelated wakeup.
        backend_->flush();

        for (pollfd& descriptor : descriptors) descriptor.revents = 0;
        const int ready = ::poll(descriptors.data(), descriptors.size(), sleepMs);
        if (ready < 0) {
            if (errno == EINTR) continue;
            Log::error("poll failed: " + Log::errnoText(errno));
            return;
        }

        if ((descriptors[kSignalFd].revents & POLLIN) != 0) {
            signalfd_siginfo info = {};
            while (::read(signalFd_, &info, sizeof(info)) == sizeof(info)) {
                Log::info(format("signal %u — shutting down", info.ssi_signo));
                running_ = false;
            }
            continue;
        }

        // Dispatched unconditionally rather than only when readable. Both
        // backends can have events already queued in userspace that no
        // descriptor will ever become readable for — Xlib buffers them on the
        // last flush, and Wayland's queue is drained by the same call.
        if (backend_->dispatchEvents()) engine_->syncOutputs();

        if ((descriptors[kControlFd].revents & POLLIN) != 0) control_.serve();

        if ((descriptors[kSessionBusFd].revents & POLLIN) != 0 && dbus::Bus::session() != nullptr) {
            dbus::Bus::session()->dispatch();
        }
        if ((descriptors[kSystemBusFd].revents & POLLIN) != 0 && dbus::Bus::system() != nullptr) {
            dbus::Bus::system()->dispatch();
        }
    }
}

int AppHost::run() {
    if (!setUp()) return 1;
    loop();
    Log::info("stopped");
    return 0;
}

std::string AppHost::handleCommand(const std::string& command,
                                   const std::vector<std::string>& arguments) {
    Library& library = engine_->library();

    auto argument = [&arguments](size_t index) -> std::string {
        return index < arguments.size() ? arguments[index] : std::string();
    };

    if (command == "status") {
        std::string out = "ok\n";
        out += "state       " + engine_->statusLine() + "\n";
        out += "backend     " + std::string(backend_->name()) +
               (backend_->supportsOcclusion() ? " (occlusion aware)"
                                              : " (cannot see other windows)") +
               "\n";
        out += "outputs     " + std::to_string(engine_->outputCount()) + "\n";
        out += "fit         " + std::string(fitModeTitle(library.fitMode())) + "\n";
        out += "memory      " + Footprint::formatted() + "\n";
        out += "tray        " + std::string(tray_.active() ? "registered" : "none") + "\n";
        out += "ffmpeg      " + ffmpeg::versionText() + "\n";
        out += "encoders    " + CodecSupport::summary() + "\n";
        out += "power gates " + engine_->power().capabilities() + "\n";
        out += "battery     ";
        if (const std::optional<double> fraction = engine_->power().batteryFraction();
            fraction.has_value()) {
            out += format("%.0f%% %s", *fraction * 100,
                          engine_->power().isOnBattery() ? "on battery" : "charging");
        } else {
            out += "none";
        }
        out += "\n";
        out += "library     " + std::to_string(library.items().size()) + " items, " +
               formatBytes(library.totalBytes()) + "\n";
        return out;
    }

    if (command == "list") {
        std::string out = "ok\n";
        const std::string selected = library.selectedId();
        if (library.items().empty()) return out + "(the library is empty)\n";
        for (const WallpaperItem& item : library.items()) {
            // The first eight characters of the id are what `play` expects, so
            // that is what is shown rather than the full 36.
            out += format("%s %-8.8s  %-28s  %s  %s\n", item.id == selected ? "*" : " ",
                          item.id.c_str(), item.title.c_str(), item.resolutionLabel().c_str(),
                          item.sizeLabel().c_str());
        }
        return out;
    }

    if (command == "play") {
        // Re-read first: `livewall add` runs in the client process and writes
        // the index behind this one's back.
        library.reload();
        const WallpaperItem* item = library.resolve(argument(0));
        if (item == nullptr) return "error no wallpaper matches \"" + argument(0) + "\"";
        const std::string title = item->title;
        engine_->select(item->id);
        return "ok playing " + title;
    }

    if (command == "stop") {
        engine_->select({});
        return "ok switched to the procedural gradient";
    }

    if (command == "reload") {
        library.reload();
        engine_->select(library.selectedId());
        return "ok reloaded";
    }

    if (command == "fit") {
        const std::string wanted = argument(0);
        if (wanted.empty()) return "error usage: fit fill|fit|stretch";
        const FitMode mode = fitModeFromString(wanted, FitMode::Fill);
        if (fitModeToString(mode) != wanted) return "error unknown fit mode \"" + wanted + "\"";
        engine_->setFitMode(mode);
        return "ok " + std::string(fitModeTitle(mode));
    }

    if (command == "preset") {
        if (argument(0).empty()) {
            std::string out = "ok\n";
            for (const TranscodePreset& preset : Transcoder::presets()) {
                out += format("%s %-12s %s\n",
                              preset.name == library.presetName() ? "*" : " ", preset.name.c_str(),
                              preset.summary().c_str());
            }
            return out;
        }
        const TranscodePreset& preset = Transcoder::presetNamed(argument(0));
        library.setPresetName(preset.name);
        return "ok import preset is " + preset.name + " (" + preset.summary() + ")";
    }

    if (command == "battery") {
        const std::string value = argument(0);
        if (value != "on" && value != "off") return "error usage: battery on|off";
        engine_->setPauseOnBattery(value == "on");
        return value == "on" ? "ok will pause on battery" : "ok will keep rendering on battery";
    }

    if (command == "autostart") {
        const std::string value = argument(0);
        if (value == "on") {
            return StartupItem::setEnabled(true) ? "ok will start with the session"
                                                 : "error could not write the autostart entry";
        }
        if (value == "off") {
            return StartupItem::setEnabled(false) ? "ok will not start with the session"
                                                  : "error could not remove the autostart entry";
        }
        return std::string("ok autostart is ") + (StartupItem::isEnabled() ? "on" : "off");
    }

    if (command == "remove") {
        library.reload();
        const WallpaperItem* item = library.resolve(argument(0));
        if (item == nullptr) return "error no wallpaper matches \"" + argument(0) + "\"";
        const std::string title = item->title;
        const bool wasSelected = item->id == library.selectedId();
        library.remove(item->id);
        if (wasSelected) engine_->select({});
        return "ok removed " + title;
    }

    if (command == "target") {
        // What `livewall add` needs from the daemon: the size and refresh of
        // the largest output, so a client process that has no display
        // connection of its own still sizes the import correctly.
        const DisplayTarget target = engine_->displayTarget();
        return format("ok %d %d %d", target.pixelWidth, target.pixelHeight, target.refreshHz);
    }

    if (command == "quit") {
        running_ = false;
        return "ok stopping";
    }

    return "error unknown command \"" + command + "\"";
}

}  // namespace livewall
