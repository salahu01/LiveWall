#include "app/Cli.h"

#include <cstdio>
#include <unistd.h>

#include "app/ControlSocket.h"
#include "import/CodecSupport.h"
#include "import/FFmpeg.h"
#include "import/Library.h"
#include "import/Transcoder.h"
#include "support/Guid.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/StartupItem.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr const char* kVersion = "1.0.0";

// Commands that are just "send this line and print what comes back". Listed so
// that an unknown one is rejected here, with the usage text, rather than
// producing a round trip and a daemon-side error.
constexpr const char* kForwarded[] = {
    "status", "list", "play", "stop", "fit", "preset", "battery", "remove", "reload", "quit",
};

bool isForwarded(const std::string& command) {
    for (const char* known : kForwarded) {
        if (command == known) return true;
    }
    return false;
}

int forward(const std::vector<std::string>& arguments) {
    std::string request;
    for (const std::string& word : arguments) {
        if (!request.empty()) request += ' ';
        request += word;
    }

    const std::optional<std::string> reply = ControlSocket::send(request);
    if (!reply.has_value()) {
        std::fputs("LiveWall is not running. Start it with `livewall` or "
                   "`systemctl --user start livewall`.\n",
                   stderr);
        return 1;
    }

    // The daemon's first word is the status; everything after it is for the
    // user. Printing the status word too would put "ok" at the top of every
    // listing.
    std::string_view body = *reply;
    const bool failed = startsWith(body, "error");
    const size_t split = body.find_first_of(" \n");
    if (split != std::string_view::npos) {
        body = body.substr(split + 1);
    } else {
        body = {};
    }

    std::FILE* stream = failed ? stderr : stdout;
    if (!body.empty()) std::fwrite(body.data(), 1, body.size(), stream);
    if (!body.empty() && body.back() != '\n') std::fputc('\n', stream);
    return failed ? 1 : 0;
}

// Asks the running daemon what to size an import against. Without one there is
// no display connection in this process at all, so the fallback is the same
// 1080p60 the other two ports use when they cannot read a screen.
DisplayTarget askDisplayTarget() {
    DisplayTarget target;

    const std::optional<std::string> reply = ControlSocket::send("target");
    if (!reply.has_value()) {
        std::fputs("LiveWall is not running, so this import is sized for 1920x1080 at 60 Hz.\n",
                   stderr);
        return target;
    }

    const std::vector<std::string> words = split(trim(*reply), ' ');
    if (words.size() < 4 || words[0] != "ok") return target;

    target.pixelWidth = std::atoi(words[1].c_str());
    target.pixelHeight = std::atoi(words[2].c_str());
    target.refreshHz = std::atoi(words[3].c_str());
    return target;
}

void reportProgress(double fraction) {
    static int lastPercent = -1;
    const int percent = static_cast<int>(fraction * 100);
    if (percent == lastPercent || percent % 5 != 0) return;
    lastPercent = percent;
    // stderr, so `livewall add … > file` still gets a clean id on stdout.
    std::fprintf(stderr, "\r  %3d%%", percent);
    std::fflush(stderr);
}

int convertCommand(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) {
        std::fputs("usage: livewall convert <source> <destination.mp4> [ultra|balanced|native]\n",
                   stderr);
        return 2;
    }

    const std::string source = paths::absolute(arguments[1]);
    const std::string destination = paths::absolute(arguments[2]);
    const TranscodePreset& preset =
        arguments.size() > 3 ? Transcoder::presetNamed(arguments[3]) : Transcoder::defaultPreset();

    const std::optional<TranscodeResult> result =
        Transcoder::convert(source, destination, preset, askDisplayTarget(), reportProgress);
    std::fputc('\n', stderr);

    if (!result.has_value()) return 1;

    std::printf("%dx%d @ %d fps, %d-bit %s, %s\n", result->width, result->height, result->fps,
                result->bitDepth, result->codec.c_str(), formatBytes(result->byteCount).c_str());
    return 0;
}

int addCommand(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        std::fputs("usage: livewall add <video> [ultra|balanced|native]\n", stderr);
        return 2;
    }

    const std::string source = paths::absolute(arguments[1]);
    if (!paths::fileExists(source)) {
        std::fprintf(stderr, "no such file: %s\n", source.c_str());
        return 1;
    }

    Library library;
    const TranscodePreset& preset = arguments.size() > 2
                                        ? Transcoder::presetNamed(arguments[2])
                                        : Transcoder::presetNamed(library.presetName());

    const std::string id = newGuidString();
    if (id.empty()) {
        std::fputs("could not generate an id\n", stderr);
        return 1;
    }
    const std::string destination = library.destinationFor(id);

    std::fprintf(stderr, "Importing %s with the %s preset (%s)\n",
                 paths::filename(source).c_str(), preset.name.c_str(), preset.summary().c_str());

    const std::optional<TranscodeResult> result =
        Transcoder::convert(source, destination, preset, askDisplayTarget(), reportProgress);
    std::fputc('\n', stderr);

    if (!result.has_value()) return 1;

    WallpaperItem item;
    item.id = id;
    item.title = paths::stem(source);
    item.filename = paths::filename(destination);
    item.width = result->width;
    item.height = result->height;
    item.fps = result->fps;
    item.bitDepth = result->bitDepth;
    item.codec = result->codec;
    item.byteCount = result->byteCount;
    item.addedAt = iso8601Now();
    library.add(item);

    std::printf("%s  %s  %s\n", item.id.c_str(), item.title.c_str(),
                item.resolutionLabel().c_str());

    // Selecting it is what the user meant by importing it. Done through the
    // daemon rather than by writing the setting here, so that the wallpaper
    // actually changes rather than changing at the next restart.
    if (ControlSocket::send("play " + id).has_value()) {
        std::fputs("Now playing.\n", stderr);
    } else {
        std::fputs("Start LiveWall to see it.\n", stderr);
    }
    return 0;
}

int autostartCommand(const std::vector<std::string>& arguments) {
    // Handled locally rather than forwarded: it has to work when the daemon is
    // not running, which is exactly when someone is setting up autostart.
    if (arguments.size() < 2) {
        std::printf("autostart is %s\n", StartupItem::isEnabled() ? "on" : "off");
        return 0;
    }
    if (arguments[1] == "on") {
        if (!StartupItem::setEnabled(true)) return 1;
        std::puts("LiveWall will start with your session.");
        return 0;
    }
    if (arguments[1] == "off") {
        if (!StartupItem::setEnabled(false)) return 1;
        std::puts("LiveWall will not start with your session.");
        return 0;
    }
    std::fputs("usage: livewall autostart [on|off]\n", stderr);
    return 2;
}

int probeCommand() {
    // Everything the app would discover at startup, without starting it. The
    // first thing to run when a wallpaper is not appearing.
    std::printf("livewall     %s\n", kVersion);
    std::printf("ffmpeg       %s\n", ffmpeg::load() ? ffmpeg::versionText().c_str() : "not found");
    std::printf("encoders     %s\n", CodecSupport::summary().c_str());
    std::printf("data         %s\n", paths::dataDirectory().c_str());
    std::printf("settings     %s\n", paths::settingsFile().c_str());
    std::printf("socket       %s\n", paths::controlSocket().c_str());
    std::printf("daemon       %s\n", ControlSocket::daemonRunning() ? "running" : "not running");
    std::printf("autostart    %s\n", StartupItem::isEnabled() ? "on" : "off");

    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11 = std::getenv("DISPLAY");
    std::printf("session      DISPLAY=%s WAYLAND_DISPLAY=%s\n", x11 != nullptr ? x11 : "(unset)",
                wayland != nullptr ? wayland : "(unset)");
    std::printf("render node  %s\n",
                ::access("/dev/dri/renderD128", R_OK | W_OK) == 0 ? "/dev/dri/renderD128"
                                                                  : "none readable");
    return 0;
}

}  // namespace

void Cli::printUsage() {
    std::puts(
        "LiveWall — a low-power live wallpaper.\n"
        "\n"
        "  livewall                       run the daemon (this is what autostart runs)\n"
        "\n"
        "  livewall status                what it is doing and why\n"
        "  livewall list                  the library; * marks what is playing\n"
        "  livewall play <id|title>       play a wallpaper (an id prefix is enough)\n"
        "  livewall stop                  back to the procedural gradient\n"
        "\n"
        "  livewall add <video> [preset]  import a video and play it\n"
        "  livewall remove <id|title>     delete a wallpaper and its file\n"
        "  livewall preset [name]         show or set the import preset\n"
        "\n"
        "  livewall fit fill|fit|stretch  how a frame maps onto a display\n"
        "  livewall battery on|off        pause rendering whenever on battery\n"
        "  livewall autostart [on|off]    start with the session\n"
        "\n"
        "  livewall convert <in> <out.mp4> [preset]   transcode without importing\n"
        "  livewall probe                 what this machine can and cannot do\n"
        "  livewall quit                  stop the daemon\n"
        "\n"
        "Presets: ultra, balanced, native.\n"
        "LIVEWALL_VERBOSE=1 makes the daemon log every gate transition.");
}

int Cli::run(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return -1;  // the daemon; main() handles it

    const std::string& command = arguments.front();

    if (command == "--help" || command == "-h" || command == "help") {
        printUsage();
        return 0;
    }
    if (command == "--version" || command == "-v" || command == "version") {
        std::puts(kVersion);
        return 0;
    }
    if (command == "probe") return probeCommand();
    if (command == "convert") return convertCommand(arguments);
    if (command == "add") return addCommand(arguments);
    if (command == "autostart") return autostartCommand(arguments);
    if (isForwarded(command)) return forward(arguments);

    std::fprintf(stderr, "unknown command: %s\n\n", command.c_str());
    printUsage();
    return 2;
}

}  // namespace livewall
