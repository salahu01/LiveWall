// The app's own memory cost, for `livewall status` and the tray menu.
//
// Reports proportional set size — Pss from /proc/self/smaps_rollup — which is
// the closest Linux has to the phys_footprint figure the macOS README quotes.
// Not RSS: an EGL client maps a large amount of the driver's shared state, and
// RSS charges this process for all of it while another GL app on the same
// machine is charged for it too. Pss splits shared pages by how many processes
// map them, so the number adds up across the system and does not flatter or
// slander the app depending on what else is running.
//
// smaps_rollup needs a kernel from 2018 (4.14). On anything older this falls
// back to RSS from statm and says so in the log once.
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
