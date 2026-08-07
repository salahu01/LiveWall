// The command line: the app's actual interface.
//
// On macOS the status-bar menu is the whole UI and on Windows it is the tray.
// Neither transfers: a Linux desktop may have no tray at all, and the ones that
// do disagree about what a tray item is allowed to do. So the CLI is the
// interface and the tray is a convenience over it, which also means the app is
// scriptable and works over ssh — both of which the other two ports are not.
//
// One binary does both jobs. With no arguments it is the daemon; with a
// subcommand it connects to a running daemon over the control socket and prints
// the reply. The two exceptions are `add` and `convert`, which transcode in
// *this* process rather than the daemon's — an import takes minutes and the
// daemon's single thread is also every output's frame pump.
#pragma once

#include <vector>
#include <string>

namespace livewall {

class Cli {
public:
    // Returns a process exit status. `arguments` excludes argv[0].
    static int run(const std::vector<std::string>& arguments);

    static void printUsage();
};

}  // namespace livewall
