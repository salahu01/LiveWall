// The control socket: how `livewall play …` reaches the running daemon.
//
// A unix socket in $XDG_RUNTIME_DIR rather than DBus, which the app already
// speaks. Three reasons, in order of weight:
//
//   It also has to work without a session. `livewall status` over ssh, from a
//   cron job, or on a machine where the session bus never started, is exactly
//   when someone needs it most. libdbus is optional here and the CLI is not.
//
//   It is the single-instance check. Two copies would mean two surfaces per
//   output and two decoders, with the second stacked invisibly over the first.
//   Connecting to the socket answers "is one already running" with no race
//   window, which a pidfile does not.
//
//   The protocol is one line in, one line out. Exposing that over DBus would
//   mean an interface, an object path and an introspection document for
//   something a user is going to pipe into grep.
//
// Wire format, deliberately dull: the client writes one line of whitespace-
// separated words and shuts down its write side; the daemon writes a reply and
// closes. First word of the reply is "ok" or "error".
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace livewall {

class ControlSocket {
public:
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;
    ControlSocket() = default;

    // Handles one request. `arguments` excludes the command word.
    using Handler = std::function<std::string(const std::string& command,
                                              const std::vector<std::string>& arguments)>;

    // Binds and listens. False when another daemon already holds it — which the
    // caller reports as "already running" rather than as a failure.
    bool listen(Handler handler);

    int fd() const { return listener_; }

    // Accepts and serves whatever is pending. Never blocks for longer than one
    // client's short timeout.
    void serve();

    // Client side. Returns the reply, or nothing when no daemon is listening.
    static std::optional<std::string> send(const std::string& request);

    // True when a daemon is listening. Used for the single-instance check
    // before doing any of the expensive startup work.
    static bool daemonRunning();

private:
    void closeListener();

    int listener_ = -1;
    std::string path_;
    Handler handler_;
};

}  // namespace livewall
