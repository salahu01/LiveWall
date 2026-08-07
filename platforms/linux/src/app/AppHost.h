// The daemon: one thread, one event loop, everything on it.
//
// No worker threads at all. The frame pump, the control socket, both DBus
// connections and the display server's events are all polled together, and the
// loop sleeps until the earliest deadline any of them has. That is a deliberate
// choice rather than a simplification:
//
//   A thread that sleeps is still a thread the scheduler wakes, and this app's
//   whole claim is that it costs nothing when nobody is looking at it. Three
//   threads parked on three descriptors is three times the wakeups.
//
//   Nothing here is long-running. The one operation that would be — transcoding
//   an imported file — deliberately does not happen in the daemon at all; the
//   CLI process does it and then tells the daemon to re-read the index. So
//   there is nothing a worker thread would be off doing.
#pragma once

#include <memory>

#include "app/ControlSocket.h"
#include "app/TrayIcon.h"
#include "app/WallpaperEngine.h"
#include "platform/Backend.h"
#include "render/EglDevice.h"

namespace livewall {

class AppHost {
public:
    AppHost();
    ~AppHost();

    // Returns a process exit status.
    int run();

private:
    bool setUp();
    void loop();
    std::string handleCommand(const std::string& command,
                              const std::vector<std::string>& arguments);
    void onBusSignal(std::string_view interface, std::string_view member, std::string_view path);

    std::unique_ptr<Backend> backend_;
    std::unique_ptr<EglDevice> egl_;
    std::unique_ptr<WallpaperEngine> engine_;
    ControlSocket control_;
    TrayIcon tray_;

    int signalFd_ = -1;
    bool running_ = true;
};

}  // namespace livewall
