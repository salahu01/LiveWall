#include "support/StartupItem.h"

#include <string>

#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// Reads the Exec= line, which is the only field that matters for reconciling.
// Not a full desktop-entry parser: the file this reads is one this app wrote.
std::string execLine(const std::string& contents) {
    for (const std::string& line : split(contents, '\n')) {
        const std::string_view trimmed = trim(line);
        if (!startsWith(trimmed, "Exec=")) continue;
        return std::string(trimmed.substr(5));
    }
    return {};
}

std::string desktopEntry(const std::string& executable) {
    // X-GNOME-Autostart-Delay holds the launch until the session is up. Without
    // it the daemon regularly starts before the compositor has published its
    // outputs, finds none, and sits there having done nothing — the outputs do
    // arrive later and are handled, but the first few seconds of every login
    // showed no wallpaper.
    //
    // NoDisplay keeps it out of application menus. This is a daemon; the tray
    // icon and the CLI are how it is reached.
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=LiveWall\n"
           "Comment=Low-power live wallpaper\n"
           "Exec=" + executable + "\n"
           "Icon=livewall\n"
           "Terminal=false\n"
           "NoDisplay=true\n"
           "X-GNOME-Autostart-enabled=true\n"
           "X-GNOME-Autostart-Delay=3\n";
}

}  // namespace

bool StartupItem::isEnabled() {
    const std::string contents = paths::readFile(paths::autostartFile());
    if (contents.empty()) return false;

    // A desktop that has been switched off through the session's own autostart
    // UI gets this line rather than having the file deleted, and reporting it
    // as enabled would make the toggle look broken.
    for (const std::string& line : split(contents, '\n')) {
        const std::string_view trimmed = trim(line);
        if (trimmed == "X-GNOME-Autostart-enabled=false" || trimmed == "Hidden=true") {
            return false;
        }
    }
    return !execLine(contents).empty();
}

bool StartupItem::setEnabled(bool enabled) {
    const std::string path = paths::autostartFile();

    if (!enabled) return paths::removeFile(path);

    const std::string executable = paths::executablePath();
    if (executable.empty()) {
        Log::error("cannot read /proc/self/exe, so there is no path to autostart");
        return false;
    }
    return paths::writeFileAtomically(path, desktopEntry(executable));
}

bool StartupItem::pointsAtThisExecutable() {
    const std::string executable = paths::executablePath();
    if (executable.empty()) return false;
    return execLine(paths::readFile(paths::autostartFile())) == executable;
}

void StartupItem::reconcile() {
    if (!isEnabled()) return;
    if (pointsAtThisExecutable()) return;

    const std::string executable = paths::executablePath();
    if (executable.empty()) return;

    Log::info("autostart entry pointed elsewhere — rewriting it to " + executable);
    paths::writeFileAtomically(paths::autostartFile(), desktopEntry(executable));
}

}  // namespace livewall
