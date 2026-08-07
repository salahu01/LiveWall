// What draws into one output's surface.
//
// Two implementations: the procedural gradient and the video decoder. The
// contract that matters is `activate`/`deactivate`, and it is the whole reason
// the app is cheap:
//
//   deactivate() must *destroy* the decode resources, not pause them. The
//   macOS port tears down the AVAssetReader and its decompression session; this
//   one closes the codec context and releases the VA-API surfaces. Suspending
//   would keep the frame pool — tens of megabytes at 10-bit — resident for as
//   long as the desktop stays covered, which is most of the day.
//
//   activate() must be able to resume from the saved timestamp. Together with
//   the above that is the entire cost model: a covered wallpaper costs the
//   memory of nothing plus one still frame the compositor is already showing.
#pragma once

#include <string>

#include "platform/Backend.h"
#include "render/FitMode.h"

namespace livewall {

class EglDevice;

class WallpaperSource {
public:
    virtual ~WallpaperSource() = default;

    // Called once, before any activate(). Returns false when this source cannot
    // run at all — a missing file, no decoder for the codec — and the engine
    // falls back to the procedural mode.
    virtual bool prepare(EglDevice& egl) = 0;

    virtual void activate() = 0;
    virtual void deactivate() = 0;
    virtual bool isActive() const = 0;

    // Frames per second this source wants to be ticked at. The engine uses the
    // largest value across active sources to size its sleep.
    virtual int framesPerSecond() const = 0;

    // Draws one frame into the current surface. Returns false when it had
    // nothing to draw, which the caller takes as "do not present" rather than
    // as an error — presenting an unchanged frame is a swap for nothing.
    virtual bool render(Surface& surface, FitMode mode) = 0;

    // Native size of the content, for the fit-mode arithmetic and the status
    // line. Zero when the source has no intrinsic size, as the gradient does
    // not.
    virtual int contentWidth() const { return 0; }
    virtual int contentHeight() const { return 0; }

    // One line for `livewall status`: "video 1920x1080 24fps vaapi" or
    // "gradient 10fps".
    virtual std::string summary() const = 0;
};

}  // namespace livewall
