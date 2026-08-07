// The app's own memory cost, for the tray menu and the log.
//
// Reports private working set — the counter Task Manager's Details tab labels
// "Memory (private working set)", and the closest analogue to the "footprint"
// figure the macOS README quotes. Notably it is not the "Memory" column of the
// Processes tab, which for a DWM-composited app folds in shared graphics pages
// this process does not own.
#pragma once

#include <cstdint>
#include <string>

namespace livewall {

class Footprint {
public:
    static std::uint64_t bytes();

    // "18.2 MB", ready to concatenate into a log line or a menu item.
    static std::string formatted();
};

}  // namespace livewall
