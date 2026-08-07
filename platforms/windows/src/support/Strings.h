// UTF-8 <-> UTF-16 and the small amount of formatting the app needs.
//
// Windows APIs are UTF-16; the on-disk index, the log and every internal string
// are UTF-8. Converting at the boundary rather than carrying wide strings
// everywhere keeps the JSON, the tests and the log free of Windows types.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace livewall {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// "12.4 MB", "930 KB" — the ByteCountFormatter equivalent, in the same
// base-1000 units the Windows shell uses for file sizes.
std::string formatBytes(std::int64_t bytes);

// Case-insensitive comparison for file extensions and registry values.
bool equalsIgnoreCase(std::string_view a, std::string_view b);

// printf into a std::string. Every call site here formats short, fixed-shape
// strings, so this stays a convenience rather than a formatting library.
std::string format(const char* fmt, ...);

}  // namespace livewall
