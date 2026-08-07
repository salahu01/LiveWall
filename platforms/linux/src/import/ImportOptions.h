// The two import choices that belong to one video rather than to the library.
//
// `TranscodePreset` carries what a user picks once and forgets — quality, size,
// bit depth. These two are different in kind: a clip shot sideways needs a
// quarter turn no other clip needs, and a slideshow of stills is fine at 8 fps
// where a drifting nebula is not. So they are per-import arguments and not
// settings.
//
// Both fields are requests rather than results. The frame rate is still capped
// to the source and still snapped to the output by `Transcoder::pacedFps`; the
// rotation is still a whole number of quarter turns. What the user picks is the
// input to those rules, not an escape from them.
//
// Header-only and free of FFmpeg types on purpose: every function here is pure
// arithmetic, so the test suite exercises it without a decoder, a display or an
// FFmpeg install.
#pragma once

#include <algorithm>
#include <string>

namespace livewall {

struct ImportOptions {
    // Frames per second to aim for, or 0 to take the preset's rate.
    //
    // Aim for, not land on: a 60 fps request against a 30 fps source is capped
    // to 30 — the frames simply are not there — and 24 against a 60 Hz output
    // still snaps to 20, because a frame arriving between two refreshes is
    // decoded and then dropped by the compositor.
    int fps = 0;

    // A quarter turn, clockwise, applied to the frame. 0, 90, 180 or 270.
    //
    // Unlike the macOS and Android ports there is no source orientation flag to
    // compose with: this port reads no display matrix, so a clip that arrives
    // sideways arrives sideways. That is precisely why the control earns its
    // place here — it is the only way to straighten such a file.
    int rotationDegrees = 0;

    // The floor is `pacedFps`'s own snapping floor: below 12 it stops looking
    // for a divisor and hands the request back unsnapped, so offering less would
    // return the judder the rest of the pipeline works to avoid. The ceiling is
    // well past anything a wallpaper wants and exists only to keep a typo out of
    // the encoder's bitrate maths.
    static constexpr int kMinimumFps = 12;
    static constexpr int kMaximumFps = 120;

    // Any integer into 0..359. C++'s % keeps the sign; this does not.
    static int normalised(int degrees) { return ((degrees % 360) + 360) % 360; }

    // Clamps a typed-in rate into kMinimumFps..kMaximumFps.
    static int sanitisedFps(int value) {
        return std::clamp(value, kMinimumFps, kMaximumFps);
    }

    // True when the turn swaps the frame's edges — a quarter turn does, a half
    // turn does not. The output sizing depends on this: a 1920x1080 clip turned
    // 90 degrees is a portrait clip and has to be fitted to the portrait edge.
    bool swapsEdges() const { return normalised(rotationDegrees) % 180 != 0; }

    // The rate to aim for, before the source cap and the display snap. Zero
    // means "no opinion", which is what an empty field says.
    int preferredFps(int presetFps) const {
        return fps > 0 ? sanitisedFps(fps) : presetFps;
    }

    // Whether the angle is one this port can actually perform. Anything that is
    // not a whole quarter turn would need a resampling filter and a decision
    // about cropping, neither of which this feature is.
    bool isQuarterTurn() const { return normalised(rotationDegrees) % 90 == 0; }

    std::string rotationLabel() const {
        const int degrees = normalised(rotationDegrees);
        return degrees == 0 ? "none" : std::to_string(degrees) + " degrees";
    }
};

}  // namespace livewall
