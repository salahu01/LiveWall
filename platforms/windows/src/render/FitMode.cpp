#include "render/FitMode.h"

#include <cmath>

#include "support/Strings.h"

namespace livewall {

std::string_view fitModeName(FitMode mode) {
    switch (mode) {
        case FitMode::Fill: return "fill";
        case FitMode::Fit: return "fit";
        case FitMode::Stretch: return "stretch";
    }
    return "fill";
}

std::string_view fitModeTitle(FitMode mode) {
    switch (mode) {
        case FitMode::Fill: return "Fill Screen";
        case FitMode::Fit: return "Fit to Screen";
        case FitMode::Stretch: return "Stretch";
    }
    return "Fill Screen";
}

std::string_view fitModeTradeoff(FitMode mode) {
    switch (mode) {
        case FitMode::Fill: return "crops edges";
        case FitMode::Fit: return "adds bars";
        case FitMode::Stretch: return "distorts";
    }
    return "";
}

FitMode fitModeFromName(std::string_view name) {
    for (const FitMode mode : kAllFitModes) {
        if (fitModeName(mode) == name) return mode;
    }
    return kDefaultFitMode;
}

FitScale fitScale(FitMode mode, double contentWidth, double contentHeight,
                  double displayWidth, double displayHeight) {
    if (mode == FitMode::Stretch) return {1.0f, 1.0f};
    if (contentWidth <= 0 || contentHeight <= 0 || displayWidth <= 0 || displayHeight <= 0) {
        return {1.0f, 1.0f};
    }

    const double contentAspect = contentWidth / contentHeight;
    const double displayAspect = displayWidth / displayHeight;
    const double ratio = contentAspect / displayAspect;

    // ratio > 1 means the content is wider than the display.
    if (mode == FitMode::Fill) {
        return ratio > 1.0 ? FitScale{static_cast<float>(ratio), 1.0f}
                           : FitScale{1.0f, static_cast<float>(1.0 / ratio)};
    }
    return ratio > 1.0 ? FitScale{1.0f, static_cast<float>(1.0 / ratio)}
                       : FitScale{static_cast<float>(ratio), 1.0f};
}

std::string fitModeEffect(FitMode mode, double contentWidth, double contentHeight,
                          double displayWidth, double displayHeight) {
    if (contentWidth <= 0 || contentHeight <= 0 || displayWidth <= 0 || displayHeight <= 0) {
        return {};
    }

    const double contentAspect = contentWidth / contentHeight;
    const double displayAspect = displayWidth / displayHeight;
    const double ratio = contentAspect / displayAspect;
    if (std::fabs(ratio - 1.0) <= 0.005) return {};

    const auto percent = [](double value) { return format("%.0f%%", value * 100.0); };

    switch (mode) {
        case FitMode::Fill:
            return ratio > 1.0
                       ? "Crops " + percent(1.0 - 1.0 / ratio) + " of the width."
                       : "Crops " + percent(1.0 - ratio) + " of the height.";
        case FitMode::Fit:
            return ratio > 1.0
                       ? "Bars above and below, " + percent(1.0 - 1.0 / ratio) + " of the height."
                       : "Bars left and right, " + percent(1.0 - ratio) + " of the width.";
        case FitMode::Stretch:
            return ratio > 1.0
                       ? "Squeezes the picture " + percent(1.0 - 1.0 / ratio) + " horizontally."
                       : "Stretches the picture " + percent(1.0 / ratio - 1.0) + " horizontally.";
    }
    return {};
}

}  // namespace livewall
