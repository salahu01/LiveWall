#include "support/Footprint.h"

#include <windows.h>
#include <psapi.h>

#include "support/Strings.h"

namespace livewall {

std::uint64_t Footprint::bytes() {
    // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 2004+) also reports private
    // *committed* bytes, but the working set is what the machine is actually
    // holding in RAM for us, which is the claim the README makes.
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return 0;
    }
    return counters.WorkingSetSize;
}

std::string Footprint::formatted() {
    const std::uint64_t value = bytes();
    if (value == 0) return "unavailable";
    return format("%.1f MB", static_cast<double>(value) / (1024.0 * 1024.0));
}

}  // namespace livewall
