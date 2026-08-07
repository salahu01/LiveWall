#include "support/DesktopVisibility.h"

#include <algorithm>
#include <array>

namespace livewall {

double DesktopVisibility::uncoveredFraction(const Rect& bounds, const std::vector<Rect>& occluders) {
    if (bounds.width() <= 0 || bounds.height() <= 0) return 1.0;

    std::array<bool, static_cast<size_t>(kGridColumns) * kGridRows> covered = {};

    const double cellWidth = bounds.width() / kGridColumns;
    const double cellHeight = bounds.height() / kGridRows;

    for (const Rect& rect : occluders) {
        // Clip to the output. A window spanning two monitors covers only the
        // part of each that it overlaps.
        const Rect clipped = {std::max(rect.left, bounds.left), std::max(rect.top, bounds.top),
                              std::min(rect.right, bounds.right),
                              std::min(rect.bottom, bounds.bottom)};
        if (clipped.width() <= 0 || clipped.height() <= 0) continue;

        for (int row = 0; row < kGridRows; ++row) {
            const double centreY = bounds.top + (row + 0.5) * cellHeight;
            if (centreY < clipped.top || centreY >= clipped.bottom) continue;
            for (int column = 0; column < kGridColumns; ++column) {
                const double centreX = bounds.left + (column + 0.5) * cellWidth;
                if (centreX < clipped.left || centreX >= clipped.right) continue;
                covered[static_cast<size_t>(row) * kGridColumns + column] = true;
            }
        }
    }

    const auto coveredCells = static_cast<double>(std::count(covered.begin(), covered.end(), true));
    return 1.0 - coveredCells / (static_cast<double>(kGridColumns) * kGridRows);
}

}  // namespace livewall
