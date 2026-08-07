// Entry point.
//
// Three modes, in the order they are checked:
//
//   LiveWall.exe                       the tray app
//   LiveWall.exe --convert in out [preset]
//                                      headless conversion, so the import
//                                      pipeline can be exercised and verified
//                                      without driving the UI
//   LiveWall.exe --probe               reports what this machine can encode and
//                                      decode, which is the first thing to ask
//                                      when an import produces H.264 instead of
//                                      HEVC
//
// Built as a GUI-subsystem executable, so there is no console window. The
// headless modes write to stderr, which is inherited when the exe is started
// from a terminal — run it as `LiveWall.exe --convert ... 2>&1 | cat` from
// PowerShell if the output needs capturing.

#include <windows.h>

// CommandLineToArgvW, which splits the wide command line the three modes above
// are selected from. It lives in shellapi.h rather than windows.h.
#include <shellapi.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "app/AppHost.h"
#include "import/CodecSupport.h"
#include "import/Transcoder.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace {

using namespace livewall;

void writeLine(const std::string& text) {
    const std::string line = text + "\n";
    // No console in a GUI-subsystem process unless one was inherited. Writing
    // to the standard handle covers the inherited case and is a harmless no-op
    // otherwise; OutputDebugString covers a debugger.
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    OutputDebugStringW(widen(line).c_str());
}

std::vector<std::wstring> commandLineArguments() {
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> arguments;
    if (raw == nullptr) return arguments;

    arguments.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) arguments.emplace_back(raw[i]);
    LocalFree(raw);
    return arguments;
}

// Another copy of this exe is already running.
//
// Two copies would mean two desktop windows per monitor and two decoders, with
// the second stacked invisibly over the first — the wallpaper would look
// correct and cost twice as much. A named mutex is the standard answer and,
// unlike scanning the process list, it is race-free: the OS decides who created
// it first.
//
// Local\ rather than Global\ deliberately: the app is per-user, and a Global
// mutex would stop a second user on the same machine from running their own
// copy in their own session.
bool alreadyRunning(HANDLE* mutexOut) {
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LiveWall.SingleInstance");
    if (mutex == nullptr) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return true;
    }
    *mutexOut = mutex;
    return false;
}

int runHeadlessConversion(const std::vector<std::wstring>& arguments) {
    // --convert <source> <destination> [ultra|balanced|native]
    size_t flag = 0;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == L"--convert") {
            flag = i;
            break;
        }
    }

    if (flag == 0 || arguments.size() < flag + 3) {
        writeLine("usage: LiveWall.exe --convert <source> <destination.mp4> "
                  "[ultra|balanced|native]");
        return 2;
    }

    const std::wstring source = arguments[flag + 1];
    const std::wstring destination = arguments[flag + 2];

    const Transcoder::Preset* preset = &Transcoder::kBalanced;
    if (arguments.size() > flag + 3) {
        const std::wstring name = arguments[flag + 3];
        if (name == L"ultra") {
            preset = &Transcoder::kUltraLight;
        } else if (name == L"native" || name == L"fidelity") {
            preset = &Transcoder::kNative;
        }
    }

    // Headless runs still size against the real panel when there is one, so
    // --convert produces the same file the menu would.
    const Transcoder::DisplayTarget display = Transcoder::DisplayTarget::primary();

    int lastReported = -1;
    Transcoder::Result result;
    const std::string error = Transcoder::convert(
        source, destination, *preset, display,
        [&lastReported](double fraction) {
            const int percent = static_cast<int>(fraction * 100);
            if (percent != lastReported && percent % 10 == 0) {
                lastReported = percent;
                writeLine(std::to_string(percent) + "%");
            }
        },
        [] { return false; }, &result);

    if (!error.empty()) {
        writeLine("error: " + error);
        return 1;
    }

    writeLine(format("%dx%d @ %d fps, %s, %s", result.width, result.height, result.fps,
                     result.codec.c_str(), formatBytes(result.byteCount).c_str()));
    return 0;
}

int runProbe() {
    if (!Transcoder::startupMediaFoundation()) {
        writeLine("Media Foundation is unavailable on this machine.");
        return 1;
    }

    writeLine(std::string("HEVC encoder: ") + (CodecSupport::hasHevcEncoder() ? "yes" : "no"));
    writeLine(std::string("HEVC decoder: ") + (CodecSupport::hasHevcDecoder() ? "yes" : "no"));
    writeLine(std::string("HEVC Main10:  ") + (CodecSupport::hasHevcMain10() ? "yes" : "no"));

    const CodecChoice choice = CodecSupport::best(10);
    writeLine("imports will use: " + choice.name);

    const std::string explanation = CodecSupport::fallbackExplanation();
    if (!explanation.empty()) writeLine(explanation);

    const Transcoder::DisplayTarget display = Transcoder::DisplayTarget::primary();
    writeLine(format("display: %dx%d @ %d Hz", display.pixelWidth, display.pixelHeight,
                     display.refreshHz));
    writeLine(format("24 fps paces to %d fps on this display",
                     Transcoder::pacedFPS(24, display.refreshHz)));
    writeLine("library: " + narrow(paths::libraryDirectory()));

    Transcoder::shutdownMediaFoundation();
    return 0;
}

}  // namespace

// The using-directive inside the anonymous namespace above does not reach out
// here — a using-directive at namespace scope applies only within that
// namespace — so the entry point names its own.
using namespace livewall;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Apartment-threaded, because the shell APIs this app calls — the file
    // picker, SHOpenFolderAndSelectItems, Shell_NotifyIcon — require it.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        Log::error("CoInitializeEx failed: " + Log::hresult(com));
        return 1;
    }

    const std::vector<std::wstring> arguments = commandLineArguments();

    int exitCode = 0;
    bool headless = false;

    for (const std::wstring& argument : arguments) {
        if (argument == L"--convert") {
            headless = true;
            exitCode = runHeadlessConversion(arguments);
            break;
        }
        if (argument == L"--probe") {
            headless = true;
            exitCode = runProbe();
            break;
        }
    }

    if (!headless) {
        HANDLE instanceMutex = nullptr;
        if (alreadyRunning(&instanceMutex)) {
            writeLine("LiveWall is already running.");
        } else {
            if (!Transcoder::startupMediaFoundation()) {
                Log::error("Media Foundation could not be started; video playback is "
                           "unavailable");
            }

            {
                AppHost host;
                exitCode = host.run();
            }

            Transcoder::shutdownMediaFoundation();
        }
        if (instanceMutex != nullptr) {
            ReleaseMutex(instanceMutex);
            CloseHandle(instanceMutex);
        }
    }

    CoUninitialize();
    return exitCode;
}
