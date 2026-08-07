#include "app/ControlSocket.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// One request and one reply are both a few hundred bytes; the cap is here so a
// confused client cannot make the daemon allocate without bound.
constexpr size_t kMaxRequestBytes = 64 * 1024;

bool fillAddress(sockaddr_un* address, const std::string& path) {
    // sun_path is 108 bytes and is not null-terminated by the kernel's
    // reckoning if it fills the array. A long $XDG_RUNTIME_DIR is unusual but
    // not impossible, and silently truncating produces a socket at a path
    // nobody can find.
    if (path.size() >= sizeof(address->sun_path)) {
        Log::error("the control socket path is too long: " + path);
        return false;
    }
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    return true;
}

std::string readAll(int socket) {
    std::string request;
    char buffer[1024];
    for (;;) {
        const ssize_t got = ::recv(socket, buffer, sizeof(buffer), 0);
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        request.append(buffer, static_cast<size_t>(got));
        if (request.size() > kMaxRequestBytes) break;
        // A client that sent one line and is waiting for the reply has not
        // closed its write side yet on every path, so a newline ends the
        // request too.
        if (request.find('\n') != std::string::npos) break;
    }
    return request;
}

void writeAll(int socket, const std::string& text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t sent = ::send(socket, text.data() + written, text.size() - written, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            return;
        }
        written += static_cast<size_t>(sent);
    }
}

int connectToDaemon() {
    const std::string path = paths::controlSocket();

    const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) return -1;

    sockaddr_un address = {};
    if (!fillAddress(&address, path)) {
        ::close(client);
        return -1;
    }

    if (::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(client);
        return -1;
    }
    return client;
}

}  // namespace

ControlSocket::~ControlSocket() { closeListener(); }

void ControlSocket::closeListener() {
    if (listener_ >= 0) {
        ::close(listener_);
        listener_ = -1;
    }
    if (!path_.empty()) {
        paths::removeFile(path_);
        path_.clear();
    }
}

bool ControlSocket::listen(Handler handler) {
    handler_ = std::move(handler);
    const std::string path = paths::controlSocket();

    // A stale socket file from a daemon that was killed rather than stopped
    // would make bind() fail with EADDRINUSE forever. Connecting first
    // distinguishes the two: a socket nobody accepts on is stale and can go.
    if (const int probe = connectToDaemon(); probe >= 0) {
        ::close(probe);
        return false;
    }
    paths::removeFile(path);

    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) {
        Log::error("cannot create the control socket: " + Log::errnoText(errno));
        return false;
    }

    sockaddr_un address = {};
    if (!fillAddress(&address, path)) {
        closeListener();
        return false;
    }

    // 0600 on the socket itself. $XDG_RUNTIME_DIR is already private, but the
    // fallback path under /tmp is not, and the umask is whatever the session
    // set.
    const mode_t previousMask = ::umask(0177);
    const int bound = ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::umask(previousMask);

    if (bound != 0) {
        Log::error("cannot bind " + path + ": " + Log::errnoText(errno));
        closeListener();
        return false;
    }

    if (::listen(listener_, 8) != 0) {
        Log::error("cannot listen on " + path + ": " + Log::errnoText(errno));
        closeListener();
        return false;
    }

    path_ = path;
    Log::info("control socket at " + path);
    return true;
}

void ControlSocket::serve() {
    if (listener_ < 0) return;

    for (;;) {
        const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;  // EAGAIN: nothing more pending

        // A client that connects and then stops talking must not hold the whole
        // app — every output's frame pump is on this thread.
        timeval timeout = {};
        timeout.tv_sec = 1;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        const std::string request = readAll(client);
        const std::vector<std::string> words = split(trim(request), ' ');

        std::string reply;
        if (words.empty()) {
            reply = "error empty request\n";
        } else if (handler_) {
            const std::vector<std::string> arguments(words.begin() + 1, words.end());
            reply = handler_(words.front(), arguments);
        }
        if (reply.empty() || reply.back() != '\n') reply += '\n';

        writeAll(client, reply);
        ::close(client);
    }
}

std::optional<std::string> ControlSocket::send(const std::string& request) {
    const int client = connectToDaemon();
    if (client < 0) return std::nullopt;

    timeval timeout = {};
    timeout.tv_sec = 5;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    writeAll(client, request + "\n");
    // Half-close so the daemon's read returns without waiting for the newline
    // to be noticed.
    ::shutdown(client, SHUT_WR);

    std::string reply;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        reply.append(buffer, static_cast<size_t>(got));
    }
    ::close(client);
    return reply;
}

bool ControlSocket::daemonRunning() {
    const int probe = connectToDaemon();
    if (probe < 0) return false;
    ::close(probe);
    return true;
}

}  // namespace livewall
