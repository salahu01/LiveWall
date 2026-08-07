// Where the app keeps things, and the handful of file operations it needs.
//
// XDG, which means the library and the settings live in *different* trees —
// unlike macOS and Windows, where both sit under one directory. That is not
// gratuitous: $XDG_DATA_HOME is what a backup tool is expected to copy and
// $XDG_CONFIG_HOME is what a dotfile repo is expected to track, and a few
// hundred megabytes of transcoded video belongs in neither of those by
// accident. Putting the library in the data tree and the six settings in the
// config tree is what every other Linux app does, and it is the behaviour a
// user's tooling already assumes.
//
//   $XDG_DATA_HOME/livewall/library/   transcoded files   (~/.local/share)
//   $XDG_DATA_HOME/livewall/index.json the library index
//   $XDG_CONFIG_HOME/livewall/settings.json                (~/.config)
//   $XDG_RUNTIME_DIR/livewall.sock     the control socket  (/run/user/N)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace livewall {
namespace paths {

// $XDG_DATA_HOME/livewall, created if absent.
std::string dataDirectory();

// ...../library — the transcoded files. Created if absent.
std::string libraryDirectory();

// ...../index.json
std::string indexFile();

// $XDG_CONFIG_HOME/livewall/settings.json
std::string settingsFile();

// $XDG_RUNTIME_DIR/livewall.sock, or a $TMPDIR path keyed to the uid when the
// runtime directory is absent — which happens over plain ssh, and is exactly
// when someone is most likely to be driving the CLI.
std::string controlSocket();

// $XDG_CONFIG_HOME/autostart/livewall.desktop
std::string autostartFile();

// Full path of the running executable, from /proc/self/exe. Empty if that
// cannot be read, which the startup-item code treats as "cannot register".
std::string executablePath();

// Filename without directory or extension — an imported wallpaper's default
// title.
std::string stem(const std::string& path);
std::string filename(const std::string& path);
std::string directory(const std::string& path);
std::string extension(const std::string& path);

std::string join(const std::string& directory, const std::string& leaf);

// Expands a leading ~ and makes the result absolute. Applied to every path
// that arrives from the command line.
std::string absolute(const std::string& path);

bool fileExists(const std::string& path);
bool createDirectories(const std::string& path);
bool removeFile(const std::string& path);
std::int64_t fileSize(const std::string& path);

// Whole-file read. Empty on any failure — callers treat an unreadable index
// the same as an absent one.
std::string readFile(const std::string& path);

// Writes to a sibling temporary and rename(2)s over the target, so a crash or
// a power loss mid-write leaves the previous index intact rather than a
// truncated one. The index is the only record that the library exists.
bool writeFileAtomically(const std::string& path, const std::string& contents);

// Hands `path` to the desktop's file manager via xdg-open. Best effort: a
// headless session has nothing to open it with and that is not an error.
void revealInFileManager(const std::string& path);

}  // namespace paths
}  // namespace livewall
