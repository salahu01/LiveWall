import AVFoundation
import Foundation

/// How a wallpaper frame is mapped onto a display whose aspect ratio doesn't
/// match it.
///
/// This is purely a property of the presentation layer, so switching modes is
/// free: no re-encode, no reload, nothing on disk changes. The library keeps
/// storing the source's own aspect ratio and the mapping happens at composite
/// time.
enum FitMode: String, CaseIterable {

    /// Scale until both axes are covered and crop the overflow. Matches what
    /// macOS's own "Fill Screen" does, and stays the default — on ambient
    /// content, losing some edge beats living with bars.
    case fill

    /// Scale until the whole frame is visible and letterbox the remainder.
    case fit

    /// Scale each axis independently. Fills the display, distorts the picture.
    case stretch

    static let `default`: FitMode = .fill

    var title: String {
        switch self {
        case .fill: return "Fill Screen"
        case .fit: return "Fit to Screen"
        case .stretch: return "Stretch"
        }
    }

    /// Trailing half of the menu label — names the cost, since every mode has one.
    var tradeoff: String {
        switch self {
        case .fill: return "crops edges"
        case .fit: return "adds bars"
        case .stretch: return "distorts"
        }
    }

    var videoGravity: AVLayerVideoGravity {
        switch self {
        case .fill: return .resizeAspectFill
        case .fit: return .resizeAspect
        case .stretch: return .resize
        }
    }

    /// What this mode actually costs for `content` shown on `display`, as a
    /// sentence for the menu's tooltip. `nil` when the aspect ratios agree
    /// closely enough that no mode does anything visible.
    ///
    /// Worth surfacing: "why is my wallpaper cropped" is answered by two aspect
    /// ratios the user can't see anywhere else in the UI.
    func effectDescription(content: CGSize, display: CGSize) -> String? {
        guard content.width > 0, content.height > 0,
              display.width > 0, display.height > 0 else { return nil }

        let contentAspect = content.width / content.height
        let displayAspect = display.width / display.height
        let ratio = contentAspect / displayAspect
        guard abs(ratio - 1) > 0.005 else { return nil }

        let percent = { (value: Double) in String(format: "%.0f%%", value * 100) }

        switch self {
        case .fill:
            return ratio > 1
                ? "Crops \(percent(1 - 1 / ratio)) of the width."
                : "Crops \(percent(1 - ratio)) of the height."
        case .fit:
            return ratio > 1
                ? "Bars above and below, \(percent(1 - 1 / ratio)) of the height."
                : "Bars left and right, \(percent(1 - ratio)) of the width."
        case .stretch:
            return ratio > 1
                ? "Squeezes the picture \(percent(1 - 1 / ratio)) horizontally."
                : "Stretches the picture \(percent(1 / ratio - 1)) horizontally."
        }
    }
}
