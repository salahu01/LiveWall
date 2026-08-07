// The small amount of string handling the app needs.
//
// Shorter than its Windows counterpart by exactly the part that made that one
// necessary. Windows APIs are UTF-16 and the index is UTF-8, so every boundary
// needs a conversion; on Linux the filesystem, the environment, the display
// server and the index are all bytes, and the app never converts anything.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace livewall {

// "12.4 MB", "930 KB" — matching macOS's ByteCountFormatter and the base-1000
// units every Linux file manager shows.
std::string formatBytes(std::int64_t bytes);

// For file extensions and DBus values. ASCII only, which is all the call sites
// compare.
bool equalsIgnoreCase(std::string_view a, std::string_view b);

bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);

std::string_view trim(std::string_view text);

// Splits on single-character separators, dropping empty fields. Used for
// /proc and /sys lines and for the control socket's wire format.
std::vector<std::string> split(std::string_view text, char separator);

// printf into a std::string. Every call site formats short, fixed-shape
// strings, so this stays a convenience rather than a formatting library.
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ISO 8601 with a Z suffix — the format Foundation's .iso8601 strategy writes,
// so an index is readable by all three ports.
std::string iso8601Now();

}  // namespace livewall
