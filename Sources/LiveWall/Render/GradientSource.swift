import AppKit
import QuartzCore

/// The cheapest wallpaper the app can draw: an animated Core Animation
/// gradient.
///
/// Nothing here renders in our process. The colour and location keyframes are
/// handed to the render server once, and WindowServer interpolates them on the
/// GPU from then on — so there is no display link, no per-frame callback, and
/// no Metal device. An idle `GradientSource` costs the app zero CPU by
/// construction rather than by carefully stopping a timer.
///
/// `deactivate()` still removes the animations: an animation the compositor is
/// interpolating for a covered desktop is wasted GPU work even though it costs
/// us nothing.
final class GradientSource: NSObject, WallpaperSource {

    private let gradient = CAGradientLayer()
    private(set) var isActive = false

    var layer: CALayer { gradient }
    var summary: String { "Gradient (compositor-driven)" }

    private static let animationKey = "livewall.drift"

    private static let paletteA: [CGColor] = [
        NSColor(srgbRed: 0.04, green: 0.05, blue: 0.11, alpha: 1).cgColor,
        NSColor(srgbRed: 0.16, green: 0.10, blue: 0.30, alpha: 1).cgColor,
        NSColor(srgbRed: 0.03, green: 0.19, blue: 0.24, alpha: 1).cgColor
    ]

    private static let paletteB: [CGColor] = [
        NSColor(srgbRed: 0.06, green: 0.04, blue: 0.16, alpha: 1).cgColor,
        NSColor(srgbRed: 0.09, green: 0.16, blue: 0.32, alpha: 1).cgColor,
        NSColor(srgbRed: 0.02, green: 0.12, blue: 0.18, alpha: 1).cgColor
    ]

    override init() {
        super.init()
        gradient.colors = Self.paletteA
        gradient.locations = [0.0, 0.5, 1.0]
        gradient.startPoint = CGPoint(x: 0, y: 0)
        gradient.endPoint = CGPoint(x: 1, y: 1)
        gradient.isOpaque = true
        gradient.needsDisplayOnBoundsChange = false
    }

    // MARK: - WallpaperSource

    func updateGeometry(size: CGSize, scale: CGFloat) {
        // Geometry changes must not restart the animation, so suppress the
        // implicit layer action rather than letting CA animate the frame.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        gradient.frame = CGRect(origin: .zero, size: size)
        gradient.contentsScale = scale
        CATransaction.commit()
    }

    func attach(to screen: NSScreen) {
        // No display link to bind — the render server owns the timing.
    }

    func activate() {
        guard !isActive else { return }
        isActive = true

        let colors = CABasicAnimation(keyPath: "colors")
        colors.fromValue = Self.paletteA
        colors.toValue = Self.paletteB
        colors.duration = 24
        colors.autoreverses = true
        colors.repeatCount = .infinity
        colors.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)

        let locations = CABasicAnimation(keyPath: "locations")
        locations.fromValue = [0.0, 0.35, 1.0]
        locations.toValue = [0.0, 0.7, 1.0]
        locations.duration = 37   // Coprime-ish with the colour cycle so the
        locations.autoreverses = true  // pair doesn't visibly repeat.
        locations.repeatCount = .infinity
        locations.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)

        let group = CAAnimationGroup()
        group.animations = [colors, locations]
        group.duration = 24 * 37
        group.repeatCount = .infinity

        gradient.add(group, forKey: Self.animationKey)
        Log.info("gradient activated — \(Footprint.formatted())")
    }

    func deactivate() {
        guard isActive else { return }
        isActive = false
        // Freeze on the current frame: copy the presented values back onto the
        // model layer before removing the animation, so nothing snaps.
        if let presented = gradient.presentation() {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            gradient.colors = presented.colors
            gradient.locations = presented.locations
            CATransaction.commit()
        }
        gradient.removeAnimation(forKey: Self.animationKey)
        Log.info("gradient deactivated — \(Footprint.formatted())")
    }
}
