#include "support/Paths.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "support/Log.h"

extern char** environ;

namespace livewall {
namespace paths {
namespace {

// An XDG variable is only honoured when it is an absolute path; the spec says
// a relative one must be treated as unset, and a $HOME-relative library
// directory would resolve differently depending on the daemon's cwd.
std::string environmentDirectory(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] != '/') return {};
    return value;
}

std::string homeDirectory() {
    if (std::string home = environmentDirectory("HOME"); !home.empty()) return home;
    // No HOME happens under systemd units with a minimal environment.
    if (const passwd* entry = ::getpwuid(::getuid()); entry != nullptr && entry->pw_dir != nullptr) {
        return entry->pw_dir;
    }
    return "/tmp";
}

std::string xdgDirectory(const char* variable, const char* fallbackLeaf) {
    if (std::string configured = environmentDirectory(variable); !configured.empty()) {
        return join(configured, "livewall");
    }
    return join(join(homeDirectory(), fallbackLeaf), "livewall");
}

}  // namespace

std::string dataDirectory() {
    const std::string path = xdgDirectory("XDG_DATA_HOME", ".local/share");
    createDirectories(path);
    return path;
}

std::string libraryDirectory() {
    const std::string path = join(dataDirectory(), "library");
    createDirectories(path);
    return path;
}

std::string indexFile() { return join(dataDirectory(), "index.json"); }

std::string settingsFile() {
    const std::string path = xdgDirectory("XDG_CONFIG_HOME", ".config");
    createDirectories(path);
    return join(path, "settings.json");
}

std::string controlSocket() {
    if (std::string runtime = environmentDirectory("XDG_RUNTIME_DIR"); !runtime.empty()) {
        return join(runtime, "livewall.sock");
    }
    // No runtime directory: a plain ssh session, or a distribution without
    // pam_systemd. /tmp is world-writable, so the uid goes in the name and the
    // socket is created with a 0600 umask by ControlSocket.
    const char* tmp = std::getenv("TMPDIR");
    const std::string base = (tmp != nullptr && tmp[0] == '/') ? tmp : "/tmp";
    return join(base, "livewall-" + std::to_string(::getuid()) + ".sock");
}

std::string autostartFile() {
    std::string base = environmentDirectory("XDG_CONFIG_HOME");
    if (base.empty()) base = join(homeDirectory(), ".config");
    const std::string directory = join(base, "autostart");
    createDirectories(directory);
    return join(directory, "livewall.desktop");
}

std::string executablePath() {
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return {};
    buffer[length] = '\0';
    return buffer;
}

std::string filename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string extension(const std::string& path) {
    const std::string leaf = filename(path);
    const size_t dot = leaf.find_last_of('.');
    // A leading dot is a hidden file, not an extension.
    if (dot == std::string::npos || dot == 0) return {};
    return leaf.substr(dot);
}

std::string stem(const std::string& path) {
    std::string leaf = filename(path);
    const size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot != 0) leaf.resize(dot);
    return leaf;
}

std::string join(const std::string& directory, const std::string& leaf) {
    if (directory.empty()) return leaf;
    if (leaf.empty()) return directory;
    if (leaf.front() == '/') return leaf;
    if (directory.back() == '/') return directory + leaf;
    return directory + "/" + leaf;
}

std::string absolute(const std::string& path) {
    if (path.empty()) return path;

    std::string expanded = path;
    if (expanded[0] == '~' && (expanded.size() == 1 || expanded[1] == '/')) {
        expanded = join(homeDirectory(), expanded.substr(1));
    }
    if (expanded[0] == '/') return expanded;

    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) return expanded;
    return join(cwd, expanded);
}

bool fileExists(const std::string& path) {
    struct stat info = {};
    return ::stat(path.c_str(), &info) == 0;
}

bool createDirectories(const std::string& path) {
    if (path.empty()) return false;

    // mkdir each component in turn. EEXIST is success, which also covers the
    // case of two LiveWall processes racing on first launch.
    std::string partial;
    partial.reserve(path.size());
    size_t index = 0;
    while (index < path.size()) {
        const size_t slash = path.find('/', index);
        const size_t stop = slash == std::string::npos ? path.size() : slash;
        partial = path.substr(0, stop);
        index = stop + 1;
        if (partial.empty()) continue;  // leading '/'
        if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) {
            Log::error("could not create " + partial + ": " + Log::errnoText(errno));
            return false;
        }
    }
    return true;
}

bool removeFile(const std::string& path) {
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::int64_t fileSize(const std::string& path) {
    struct stat info = {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<std::int64_t>(info.st_size);
}

std::string readFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return {};

    std::string contents;
    char buffer[8192];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        contents.append(buffer, got);
    }
    std::fclose(file);
    return contents;
}

bool writeFileAtomically(const std::string& path, const std::string& contents) {
    const std::string temporary = path + ".tmp";

    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        Log::error("could not write " + temporary + ": " + Log::errnoText(errno));
        return false;
    }

    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t chunk = ::write(fd, contents.data() + written, contents.size() - written);
        if (chunk <= 0) {
            if (errno == EINTR) continue;
            Log::error("could not write " + temporary + ": " + Log::errnoText(errno));
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<size_t>(chunk);
    }

    // Without the fsync, rename(2) only guarantees that the *name* change is
    // ordered — the new file's contents may still be in flight, and a power
    // loss can leave the index a correctly-named zero-length file. That is
    // strictly worse than the truncated write this function exists to prevent.
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        ::unlink(temporary.c_str());
        return false;
    }

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        Log::error("could not replace " + path + ": " + Log::errnoText(errno));
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

void revealInFileManager(const std::string& path) {
    const char* argv[] = {"xdg-open", path.c_str(), nullptr};
    pid_t child = 0;
    // posix_spawnp rather than system(): no shell, so a filename with a quote
    // or a space in it is not a problem to think about.
    const int result = ::posix_spawnp(&child, "xdg-open", nullptr, nullptr,
                                      const_cast<char* const*>(argv), environ);
    if (result != 0) {
        Log::info("no xdg-open available");
        return;
    }
    // Detached: waiting would block the event loop on a file manager launch.
    // The double-fork the shell would need is not worth it — the child is
    // reaped by the SIGCHLD handler AppHost installs.
}

}  // namespace paths
}  // namespace livewall
