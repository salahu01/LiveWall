#include "support/StartupItem.h"

#include <windows.h>

#include <string>

#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"LiveWall";

// The command line stored in the Run key. Quoted, because a path with a space
// in it — "C:\Program Files\LiveWall\LiveWall.exe" — is otherwise parsed as a
// command plus arguments, and Windows would try to run "C:\Program".
std::wstring commandLine() {
    const std::wstring exe = paths::executablePath();
    if (exe.empty()) return {};
    return L"\"" + exe + L"\"";
}

std::wstring readValue() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring value;
    if (RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > 0) {
        value.resize(bytes / sizeof(wchar_t));
        if (RegQueryValueExW(key, kValueName, nullptr, nullptr,
                             reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
            value.clear();
        } else {
            // The registry stores the terminator inside the value; leaving it
            // in makes every string comparison below fail for no visible reason.
            while (!value.empty() && value.back() == L'\0') value.pop_back();
        }
    }

    RegCloseKey(key);
    return value;
}

}  // namespace

bool StartupItem::isEnabled() { return !readValue().empty(); }

bool StartupItem::pointsAtThisExecutable() {
    const std::wstring stored = readValue();
    if (stored.empty()) return false;

    const std::wstring expected = commandLine();
    if (expected.empty()) return false;

    // Paths are case-insensitive on Windows, and the value may or may not have
    // been written by this build.
    return CompareStringOrdinal(stored.c_str(), -1, expected.c_str(), -1,
                                /*ignoreCase=*/TRUE) == CSTR_EQUAL;
}

bool StartupItem::setEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        Log::error("could not open the Run key: " + Log::lastError());
        return false;
    }

    bool ok = false;
    if (enabled) {
        const std::wstring command = commandLine();
        if (command.empty()) {
            Log::error("could not resolve this executable's path");
        } else {
            const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            ok = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                bytes) == ERROR_SUCCESS;
            if (ok) Log::info("start at login enabled: " + narrow(command));
        }
    } else {
        const LSTATUS status = RegDeleteValueW(key, kValueName);
        // Deleting something that was already absent is the outcome asked for.
        ok = (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
        if (ok) Log::info("start at login disabled");
    }

    RegCloseKey(key);
    if (!ok) Log::error("could not update the Run key: " + Log::lastError());
    return ok;
}

void StartupItem::reconcile() {
    if (!isEnabled()) return;
    if (pointsAtThisExecutable()) return;

    Log::info("start-at-login points elsewhere — repointing it at this copy");
    setEnabled(true);
}

}  // namespace livewall
