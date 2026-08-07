// How a wallpaper frame is mapped onto an output whose aspect ratio doesn't
// match it.
//
// Purely a property of the presentation layer, so switching modes is free: no
// re-encode, no reload, nothing on disk changes. The library keeps storing the
// source's own aspect ratio and the mapping happens at draw time — here, as
// four floats handed to the vertex shader.
#pragma once

#include <string>
#include <string_view>

namespace livewall {

enum class FitMode {
    // Scale until both axes are covered and crop the overflow. The default: on
    // ambient content, losing some edge beats living with bars.
    Fill,
    // Scale until the whole frame is visible and letterbox the remainder.
    Fit,
    // Scale each axis independently. Fills the output, distorts the picture.
    Stretch,
};

// The texture-coordinate mapping the shader applies: uv' = uv * scale + offset.
//
// Expressed this way rather than as a vertex transform because Fit has to be
// able to produce coordinates *outside* [0,1], which is what the fragment
// shader keys the transparent letterbox off. Moving the vertices instead would
// leave the bars as whatever was in the framebuffer before.
struct FitTransform {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

FitMode fitModeFromString(std::string_view text, FitMode fallback = FitMode::Fill);
std::string_view fitModeToString(FitMode mode);
std::string_view fitModeTitle(FitMode mode);

// Trailing half of a menu label — names the cost, since every mode has one.
std::string_view fitModeTradeoff(FitMode mode);

FitTransform fitTransform(FitMode mode, int contentWidth, int contentHeight, int targetWidth,
                          int targetHeight);

// What this mode actually costs for `content` shown on `target`, as a sentence.
// Empty when the aspect ratios agree closely enough that no mode does anything
// visible.
//
// Worth surfacing: "why is my wallpaper cropped" is answered by two aspect
// ratios the user cannot see anywhere else.
std::string fitModeEffect(FitMode mode, int contentWidth, int contentHeight, int targetWidth,
                          int targetHeight);

}  // namespace livewall
