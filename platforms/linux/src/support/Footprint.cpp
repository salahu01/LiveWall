#include "support/Footprint.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

std::uint64_t readPss() {
    std::FILE* file = std::fopen("/proc/self/smaps_rollup", "r");
    if (file == nullptr) return 0;

    std::uint64_t kilobytes = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        // "Pss:            12345 kB" — and only that key, since Pss_Anon and
        // Pss_File below it would double-count.
        if (std::strncmp(line, "Pss:", 4) != 0) continue;
        kilobytes = std::strtoull(line + 4, nullptr, 10);
        break;
    }
    std::fclose(file);
    return kilobytes * 1024;
}

std::uint64_t readRss() {
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) return 0;

    unsigned long long total = 0;
    unsigned long long resident = 0;
    const int fields = std::fscanf(file, "%llu %llu", &total, &resident);
    std::fclose(file);
    if (fields != 2) return 0;

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    return resident * static_cast<std::uint64_t>(pageSize > 0 ? pageSize : 4096);
}

}  // namespace

std::uint64_t Footprint::bytes() {
    if (const std::uint64_t pss = readPss(); pss > 0) return pss;

    static bool warned = false;
    if (!warned) {
        warned = true;
        Log::info("no smaps_rollup on this kernel — reporting RSS, which over-counts shared "
                  "driver pages");
    }
    return readRss();
}

std::string Footprint::formatted() {
    const double megabytes = static_cast<double>(bytes()) / (1024.0 * 1024.0);
    return format("%.1f MB", megabytes);
}

}  // namespace livewall
