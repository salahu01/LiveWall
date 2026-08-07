#include "support/Guid.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <sys/random.h>

#include "support/Strings.h"

namespace livewall {
namespace {

// getrandom(2) rather than /dev/urandom: no file descriptor to run out of, no
// open() that can fail inside a chroot, and it cannot return short before the
// pool is initialised. Falls back to the device only on an ancient kernel.
bool randomBytes(std::uint8_t* out, size_t count) {
    size_t filled = 0;
    while (filled < count) {
        const ssize_t got = ::getrandom(out + filled, count - filled, 0);
        if (got <= 0) break;
        filled += static_cast<size_t>(got);
    }
    if (filled == count) return true;

    std::FILE* device = std::fopen("/dev/urandom", "rb");
    if (device == nullptr) return false;
    const size_t read = std::fread(out + filled, 1, count - filled, device);
    std::fclose(device);
    return filled + read == count;
}

}  // namespace

std::string newGuidString() {
    std::uint8_t bytes[16] = {};
    if (!randomBytes(bytes, sizeof(bytes))) return {};

    // Version 4, variant 1 — the same shape Foundation's UUID() and Windows's
    // CoCreateGuid produce, so an index written by any of the three ports keys
    // the same way.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    return format("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                  bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                  bytes[14], bytes[15]);
}

bool isGuidString(std::string_view text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const bool hyphenPosition = (i == 8 || i == 13 || i == 18 || i == 23);
        if (hyphenPosition) {
            if (text[i] != '-') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace livewall
