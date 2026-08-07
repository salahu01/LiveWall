#include "support/Strings.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <vector>

namespace livewall {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        result.data(), needed);
    return result;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                           static_cast<int>(utf16.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

std::string formatBytes(std::int64_t bytes) {
    if (bytes < 0) bytes = 0;
    // Base 1000, matching what File Explorer reports and what
    // ByteCountFormatter's .file style gives on the macOS side, so the two
    // apps' menus agree about the size of the same library.
    static const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < std::size(units)) {
        value /= 1000.0;
        ++unit;
    }
    return unit == 0 ? format("%lld bytes", static_cast<long long>(bytes))
                     : format("%.1f %s", value, units[unit]);
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(a[i]);
        const unsigned char rhs = static_cast<unsigned char>(b[i]);
        // ASCII-only on purpose: the only callers are file extensions and
        // registry key names, and a locale-aware fold would be both slower and
        // surprising (Turkish dotless i turns ".MP4" into something else).
        const unsigned char lo = (lhs >= 'A' && lhs <= 'Z') ? lhs + 32 : lhs;
        const unsigned char ro = (rhs >= 'A' && rhs <= 'Z') ? rhs + 32 : rhs;
        if (lo != ro) return false;
    }
    return true;
}

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, probe);
    va_end(probe);

    std::string result;
    if (needed > 0) {
        result.resize(static_cast<size_t>(needed));
        std::vsnprintf(result.data(), static_cast<size_t>(needed) + 1, fmt, args);
    }
    va_end(args);
    return result;
}

}  // namespace livewall
