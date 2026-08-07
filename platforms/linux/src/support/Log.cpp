#include "support/Log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace livewall {
namespace {

// One write(2) per line rather than fprintf, so a line from the render thread
// cannot interleave with one from the transcoder. stderr is line-buffered only
// when it is a terminal, and under systemd it is a pipe.
void emit(std::string_view prefix, std::string_view message) {
    std::string line;
    line.reserve(prefix.size() + message.size() + 2);
    line.append(prefix);
    line.append(message);
    line.push_back('\n');
    // Nothing useful to do if the log itself fails — an error path inside the
    // error path is how a logger deadlocks. The result is read into a discarded
    // variable rather than cast to void, because glibc marks write() as
    // warn_unused_result and a cast does not satisfy it.
    const ssize_t ignored = ::write(STDERR_FILENO, line.data(), line.size());
    static_cast<void>(ignored);
}

}  // namespace

bool Log::verbose() {
    static const bool value = std::getenv("LIVEWALL_VERBOSE") != nullptr;
    return value;
}

void Log::info(std::string_view message) {
    if (!verbose()) return;
    emit("[livewall] ", message);
}

void Log::error(std::string_view message) {
    emit("[livewall] ERROR ", message);
}

std::string Log::errnoText(int value) {
    char buffer[256] = {};
    // The GNU strerror_r returns the pointer and may not touch the buffer.
    const char* text = ::strerror_r(value, buffer, sizeof(buffer));
    return std::string(text) + " (" + std::to_string(value) + ")";
}

}  // namespace livewall
