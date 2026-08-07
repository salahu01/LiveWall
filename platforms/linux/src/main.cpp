// One binary, two jobs.
//
//   livewall              the daemon
//   livewall <command>    the client
//
// Split here rather than in two executables so that a user has one thing to
// install, one thing to put in autostart and one thing to look up in `which`.

#include <string>
#include <vector>

#include "app/AppHost.h"
#include "app/Cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);

    if (!arguments.empty()) {
        const int status = livewall::Cli::run(arguments);
        // -1 is the CLI saying "this was not a command for me". It cannot
        // happen with a non-empty argument list, and returning it as an exit
        // status would be a confusing 255.
        if (status >= 0) return status;
    }

    livewall::AppHost host;
    return host.run();
}
