// Logging, off unless asked for.
//
// Same switch as the other two ports — LIVEWALL_VERBOSE=1 — writing to stderr
// and nowhere else. No syslog, no journal socket: the daemon is normally
// started by a systemd user unit, and systemd already captures stderr into the
// journal. Opening a second path to the same place would only add a buffer.
//
// Errors always go out. Info is gated, because an app whose entire point is
// costing nothing should not be formatting strings on a timer.
#pragma once

#include <string>
#include <string_view>

namespace livewall {

class Log {
public:
    // True when LIVEWALL_VERBOSE is set in the environment. Read once.
    static bool verbose();

    static void info(std::string_view message);
    static void error(std::string_view message);

    // "Permission denied (13)" from an errno. Most of what goes wrong here is
    // a syscall, and the bare number is unreadable.
    static std::string errnoText(int value);
};

}  // namespace livewall
