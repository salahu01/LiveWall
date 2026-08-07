#include "support/Log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>

#include "support/Strings.h"

namespace livewall {
namespace {

std::mutex g_mutex;

void write(std::string_view level, std::string_view message) {
    std::string line;
    line.reserve(message.size() + level.size() + 16);
    line += "LiveWall ";
    line += level;
    line += ": ";
    line += message;
    line += "\n";

    // One lock for both sinks: interleaved half-lines from the decode thread
    // and the UI thread are worse than the cost of a mutex on a path that only
    // runs when verbose logging is on.
    std::lock_guard<std::mutex> guard(g_mutex);
    OutputDebugStringW(widen(line).c_str());
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace

bool Log::verbose() {
    // Read once and cached: this is consulted on the frame pump.
    static const bool value = [] {
        wchar_t buffer[8]{};
        return GetEnvironmentVariableW(L"LIVEWALL_VERBOSE", buffer,
                                       static_cast<DWORD>(std::size(buffer))) > 0;
    }();
    return value;
}

void Log::info(std::string_view message) {
    if (!verbose()) return;
    write("info", message);
}

void Log::error(std::string_view message) {
    write("error", message);
}

std::string Log::hresult(long hr) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));

    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);

    std::string result = code;
    if (length > 0 && text != nullptr) {
        std::wstring_view view(text, length);
        // FormatMessage terminates its strings with CRLF, which would break a
        // one-line log entry in two.
        while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n' || view.back() == L' ')) {
            view.remove_suffix(1);
        }
        result += " (";
        result += narrow(view);
        result += ")";
    }
    if (text != nullptr) LocalFree(text);
    return result;
}

std::string Log::lastError() {
    return hresult(static_cast<long>(HRESULT_FROM_WIN32(GetLastError())));
}

}  // namespace livewall
