import AppKit
import QuartzCore

/// A renderer that draws into a desktop window's layer.
///
/// The contract that makes the whole app cheap:
///
/// - `activate()` builds whatever decode/GPU resources are needed and starts drawing.
/// - `deactivate()` **releases those resources** rather than merely pausing a
///   timer, and leaves the last drawn frame on screen so re-activating never
///   flashes black. A deactivated source must cost zero CPU and zero GPU.
protocol WallpaperSource: AnyObject {
    /// Layer handed to the desktop window. Created once, outlives activate/deactivate.
    var layer: CALayer { get }

    var isActive: Bool { get }

    /// Called when the layer's backing screen geometry changes.
    func updateGeometry(size: CGSize, scale: CGFloat)

    /// Binds the source to a screen so it can drive a display link off that
    /// display's refresh rather than a wall-clock timer.
    func attach(to screen: NSScreen)

    func activate()
    func deactivate()

    /// How the frame maps onto a display of a different aspect ratio.
    func setFitMode(_ mode: FitMode)

    /// Short label for the status menu.
    var summary: String { get }
}

extension WallpaperSource {
    /// Procedural sources draw to whatever bounds they are handed, so they have
    /// no aspect ratio of their own and nothing to fit.
    func setFitMode(_ mode: FitMode) {}
}
