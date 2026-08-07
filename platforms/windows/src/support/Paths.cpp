#include "support/Paths.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <vector>

#include "support/Log.h"
#include "support/Strings.h"

namespace livewall::paths {
namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
        if (raw != nullptr) CoTaskMemFree(raw);
        return {};
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

}  // namespace

std::wstring appDataDirectory() {
    static const std::wstring directory = [] {
        const std::wstring base = knownFolder(FOLDERID_LocalAppData);
        if (base.empty()) {
            // No LocalAppData is a broken profile, but falling back to the
            // executable's own directory at least keeps a portable copy
            // working from a USB stick.
            Log::error("LocalAppData is unavailable; using the executable directory");
            return executableDirectory();
        }
        std::wstring path = join(base, L"LiveWall");
        createDirectories(path);
        return path;
    }();
    return directory;
}

std::wstring libraryDirectory() {
    static const std::wstring directory = [] {
        std::wstring path = join(appDataDirectory(), L"library");
        createDirectories(path);
        return path;
    }();
    return directory;
}

std::wstring indexFile() { return join(appDataDirectory(), L"index.json"); }
std::wstring settingsFile() { return join(appDataDirectory(), L"settings.json"); }

std::wstring executablePath() {
    static const std::wstring path = [] {
        // MAX_PATH is not the limit on a long-paths-enabled system, so grow
        // until the call stops truncating rather than assuming.
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD written =
                GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0) return std::wstring();
            if (written < buffer.size() - 1) return std::wstring(buffer.data(), written);
            buffer.resize(buffer.size() * 2);
        }
    }();
    return path;
}

std::wstring executableDirectory() {
    const std::wstring path = executablePath();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring filename(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring stem(const std::wstring& path) {
    std::wstring name = filename(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring join(const std::wstring& directory, const std::wstring& leaf) {
    if (directory.empty()) return leaf;
    if (leaf.empty()) return directory;
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + leaf;
    return directory + L'\\' + leaf;
}

bool fileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool createDirectories(const std::wstring& path) {
    if (path.empty()) return false;
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
           result == ERROR_FILE_EXISTS;
}

bool removeFile(const std::wstring& path) {
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != 0;
}

std::int64_t fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) return 0;
    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart;
}

std::string readFile(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 ||
        size.QuadPart > 64ll * 1024 * 1024) {
        // A 64 MB ceiling on the index: anything larger is not something this
        // app wrote, and reading it would be the only unbounded allocation in
        // the process.
        CloseHandle(file);
        return {};
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                             &read, nullptr) != 0;
    CloseHandle(file);
    if (!ok) return {};
    contents.resize(read);
    return contents;
}

bool writeFileAtomically(const std::wstring& path, const std::string& contents) {
    const std::wstring temporary = path + L".tmp";

    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log::error("could not open the index for writing: " + Log::lastError());
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                        &written, nullptr) != 0 &&
              written == contents.size();
    // Without this the rename can complete while the data is still in the cache,
    // which is the exact window a power loss turns into a zero-length index.
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);

    if (!ok) {
        removeFile(temporary);
        return false;
    }

    // MoveFileEx with REPLACE_EXISTING is atomic within a volume, so a reader
    // sees either the old index or the new one and never a partial write.
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        Log::error("could not replace the index: " + Log::lastError());
        removeFile(temporary);
        return false;
    }
    return true;
}

void revealInExplorer(const std::wstring& path) {
    // Selecting the item is better than opening the folder: it works whether
    // `path` is the library directory or a file inside it.
    PIDLIST_ABSOLUTE id = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id != nullptr) {
        SHOpenFolderAndSelectItems(id, 0, nullptr, 0);
        CoTaskMemFree(id);
        return;
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace livewall::paths
