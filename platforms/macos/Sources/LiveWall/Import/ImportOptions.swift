import CoreGraphics
import Foundation

/// The two choices that belong to one video rather than to the library.
///
/// `Transcoder.Preset` carries what a user picks once and forgets — quality,
/// size, bit depth. These two are different in kind: a clip shot sideways needs
/// a quarter turn no other clip needs, and a slideshow of stills is fine at
/// 8 fps where a drifting nebula is not. The open panel is the only place either
/// question can be answered, so they live here and not in the library.
///
/// Both fields are requests rather than results. The frame rate is still capped
/// to the source and still snapped to the display by `Transcoder.pacedFPS`; the
/// rotation is still composed with the track's own `preferredTransform`. What
/// the user picks is the input to those rules, not an escape from them.
struct ImportOptions: Equatable {

    /// Frames per second to aim for, or `nil` to take the preset's rate.
    ///
    /// Aim for, not land on: a 60 fps request against a 30 fps source is capped
    /// to 30 — the frames simply are not there — and 24 against a 60 Hz panel
    /// still snaps to 20, because a frame arriving between two refreshes is
    /// decoded and then dropped by the compositor.
    var fps: Int?

    /// A quarter turn applied on top of the track's own orientation.
    ///
    /// Additive rather than absolute: a portrait clip already carrying a 90°
    /// `preferredTransform` and rotated another 90° here ends up at 180°, which
    /// is what someone pressing "rotate" twice expects. Setting it absolutely
    /// would silently undo the track's own orientation on the first press.
    var rotationDegrees: Int = 0

    static let `default` = ImportOptions()

    /// The floor is `pacedFPS`'s own snapping floor: below 12 it stops looking
    /// for a divisor and hands the request back unsnapped, so offering less
    /// would return the judder the rest of the pipeline works to avoid. The
    /// ceiling is well past anything a wallpaper wants and exists only to keep a
    /// typo out of the encoder's bitrate maths.
    static let minimumFPS = 12
    static let maximumFPS = 120

    /// The quarter turns offered, in the order a "rotate" control walks.
    static let rotations = [0, 90, 180, 270]

    /// The rate to aim for, before the source cap and the display snap.
    func preferredFPS(for preset: Transcoder.Preset) -> Int {
        guard let fps else { return preset.fps }
        return Self.sanitisedFPS(fps)
    }

    /// The user's quarter turn as a transform, to concatenate onto the track's
    /// `preferredTransform`.
    ///
    /// Concatenating rather than replacing is what makes the turn additive, and
    /// it also means the caller needs no special case for 0°: the identity
    /// transform composes away to nothing.
    var rotationTransform: CGAffineTransform {
        let degrees = Self.normalised(rotationDegrees)
        guard degrees != 0 else { return .identity }
        return CGAffineTransform(rotationAngle: CGFloat(degrees) * .pi / 180)
    }

    /// Whether the turn swaps the output's edges. A quarter turn does; a half
    /// turn does not.
    var swapsEdges: Bool { Self.normalised(rotationDegrees) % 180 != 0 }

    /// Label for the picker's rotation control.
    var rotationLabel: String {
        Self.normalised(rotationDegrees) == 0 ? "None" : "\(Self.normalised(rotationDegrees))°"
    }

    /// Clamps a typed-in rate into `minimumFPS...maximumFPS`.
    static func sanitisedFPS(_ value: Int) -> Int {
        min(max(value, minimumFPS), maximumFPS)
    }

    /// Any integer into 0..<360. Swift's `%` keeps the sign; this does not.
    static func normalised(_ degrees: Int) -> Int {
        ((degrees % 360) + 360) % 360
    }
}
