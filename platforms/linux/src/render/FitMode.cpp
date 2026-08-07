#include "render/FitMode.h"

#include <cmath>

#include "support/Strings.h"

namespace livewall {
namespace {

std::string percent(double value) { return format("%.0f%%", value * 100.0); }

// Below this the two aspect ratios are close enough that no mode does anything
// a person would notice, and every mode's description would read as a lie about
// a 0% crop.
constexpr double kIndistinguishable = 0.005;

}  // namespace

FitMode fitModeFromString(std::string_view text, FitMode fallback) {
    if (equalsIgnoreCase(text, "fill")) return FitMode::Fill;
    if (equalsIgnoreCase(text, "fit")) return FitMode::Fit;
    if (equalsIgnoreCase(text, "stretch")) return FitMode::Stretch;
    return fallback;
}

std::string_view fitModeToString(FitMode mode) {
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
    return "crops edges";
}

FitTransform fitTransform(FitMode mode, int contentWidth, int contentHeight, int targetWidth,
                          int targetHeight) {
    FitTransform transform;
    if (contentWidth <= 0 || contentHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
        return transform;
    }

    const double contentAspect = static_cast<double>(contentWidth) / contentHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
    // Above 1 the content is relatively wider than the output; below 1, taller.
    const double ratio = contentAspect / targetAspect;

    if (mode == FitMode::Stretch || std::fabs(ratio - 1.0) <= kIndistinguishable) {
        return transform;
    }

    if (mode == FitMode::Fill) {
        // Cover: the overflowing axis is sampled from a narrower window of the
        // texture, centred, so the edges fall outside and are cropped.
        if (ratio > 1.0) {
            transform.scaleX = static_cast<float>(1.0 / ratio);
            transform.offsetX = static_cast<float>((1.0 - 1.0 / ratio) / 2.0);
        } else {
            transform.scaleY = static_cast<float>(ratio);
            transform.offsetY = static_cast<float>((1.0 - ratio) / 2.0);
        }
        return transform;
    }

    // Fit: the content occupies the middle 1/ratio of the constrained axis, and
    // the sampled coordinate runs past [0,1] in the bars, which is what makes
    // them transparent.
    if (ratio > 1.0) {
        transform.scaleY = static_cast<float>(ratio);
        transform.offsetY = static_cast<float>(-(ratio - 1.0) / 2.0);
    } else {
        transform.scaleX = static_cast<float>(1.0 / ratio);
        transform.offsetX = static_cast<float>(-(1.0 / ratio - 1.0) / 2.0);
    }
    return transform;
}

std::string fitModeEffect(FitMode mode, int contentWidth, int contentHeight, int targetWidth,
                          int targetHeight) {
    if (contentWidth <= 0 || contentHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
        return {};
    }

    const double contentAspect = static_cast<double>(contentWidth) / contentHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
    const double ratio = contentAspect / targetAspect;
    if (std::fabs(ratio - 1.0) <= kIndistinguishable) return {};

    switch (mode) {
        case FitMode::Fill:
            return ratio > 1.0 ? "Crops " + percent(1.0 - 1.0 / ratio) + " of the width."
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
