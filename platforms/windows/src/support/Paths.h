// Where the app keeps things, and the handful of file operations it needs.
//
// Everything lives under %LOCALAPPDATA%\LiveWall — not %APPDATA%. Roaming
// profiles copy their contents between machines at sign-in, and a library of
// transcoded video is exactly what should not be dragged across a network. The
// macOS equivalent, Application Support, is likewise excluded from iCloud.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace livewall {

namespace paths {

// %LOCALAPPDATA%\LiveWall, created if absent.
std::wstring appDataDirectory();

// ...\library — the transcoded files. Created if absent.
std::wstring libraryDirectory();

// ...\index.json
std::wstring indexFile();

// ...\settings.json
std::wstring settingsFile();

// Full path of the running executable.
std::wstring executablePath();

// The directory holding the executable, with no trailing separator.
std::wstring executableDirectory();

// Filename without directory or extension — used as an imported wallpaper's
// default title.
std::wstring stem(const std::wstring& path);

std::wstring filename(const std::wstring& path);

std::wstring join(const std::wstring& directory, const std::wstring& leaf);

bool fileExists(const std::wstring& path);
bool createDirectories(const std::wstring& path);
bool removeFile(const std::wstring& path);
std::int64_t fileSize(const std::wstring& path);

// Whole-file read as UTF-8 bytes. Empty on any failure — callers treat an
// unreadable index the same as an absent one.
std::string readFile(const std::wstring& path);

// Writes through a temporary file and then replaces the target, so a crash or a
// power loss mid-write leaves the previous index intact rather than a truncated
// one. The index is the only record that the library exists.
bool writeFileAtomically(const std::wstring& path, const std::string& contents);

// Opens Explorer with `path` selected.
void revealInExplorer(const std::wstring& path);

}  // namespace paths
}  // namespace livewall
