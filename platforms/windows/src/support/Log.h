// Logging, off unless asked for.
//
// The macOS app writes to os_log and turns verbose output on with
// LIVEWALL_VERBOSE=1. Same switch here, writing to two places: stderr, which is
// inherited when the exe is started from a terminal, and OutputDebugString,
// which is where a GUI-subsystem process's output is actually visible (DebugView,
// or the Visual Studio output window).
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

    // Formats a Windows HRESULT as "0x80070005 (Access is denied)". COM
    // failures are otherwise unreadable and this is most of what goes wrong.
    static std::string hresult(long hr);

    // Formats the last GetLastError() the same way.
    static std::string lastError();
};

}  // namespace livewall
