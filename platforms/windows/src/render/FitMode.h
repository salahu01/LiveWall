// How a wallpaper frame is mapped onto a display whose aspect ratio does not
// match it.
//
// Purely a property of presentation, so switching modes is free: no re-encode,
// no reload, nothing on disk changes. The library keeps storing the source's own
// aspect ratio and the mapping happens in the vertex shader at draw time.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace livewall {

enum class FitMode {
    // Scale until both axes are covered and crop the overflow. Matches what
    // Windows's own "Fill" wallpaper setting does, and stays the default — on
    // ambient content, losing some edge beats living with bars.
    Fill = 0,
    // Scale until the whole frame is visible and letterbox the remainder.
    Fit = 1,
    // Scale each axis independently. Fills the display, distorts the picture.
    Stretch = 2,
};

inline constexpr FitMode kDefaultFitMode = FitMode::Fill;
inline constexpr std::array<FitMode, 3> kAllFitModes{FitMode::Fill, FitMode::Fit,
                                                     FitMode::Stretch};

std::string_view fitModeName(FitMode mode);      // "fill" — what goes in settings.json
std::string_view fitModeTitle(FitMode mode);     // "Fill Screen"
std::string_view fitModeTradeoff(FitMode mode);  // "crops edges"

FitMode fitModeFromName(std::string_view name);

// The scale applied to the video's normalised quad for this mode, given the
// content and display aspect ratios. Returned as an (x, y) pair the vertex
// shader multiplies its clip-space position by.
//
// Fill returns values >= 1 on the axis that overflows, and the shader's texture
// coordinates clamp — so the overflow is cropped rather than drawn outside the
// viewport. Fit returns values <= 1, leaving bars. Stretch always returns
// (1, 1).
struct FitScale {
    float x = 1.0f;
    float y = 1.0f;
};

FitScale fitScale(FitMode mode, double contentWidth, double contentHeight,
                  double displayWidth, double displayHeight);

// What this mode actually costs for `content` shown on `display`, as a sentence
// for the menu's tooltip. Empty when the aspect ratios agree closely enough
// that no mode does anything visible.
//
// Worth surfacing: "why is my wallpaper cropped" is answered by two aspect
// ratios the user cannot see anywhere else in the UI.
std::string fitModeEffect(FitMode mode, double contentWidth, double contentHeight,
                          double displayWidth, double displayHeight);

}  // namespace livewall
