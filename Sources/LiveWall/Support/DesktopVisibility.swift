import AppKit
import CoreGraphics

/// How much of a display's desktop is actually left uncovered.
///
/// `NSWindow.occlusionState` answers this as a yes/no: a window covering all but
/// a sliver of the desktop still reports the wallpaper as visible, and we decode
/// at full rate for that sliver. The system has no API for the fraction, so it
/// gets derived from the window list.
///
/// Only window *geometry* is read — bounds, layer and alpha. That needs no
/// Screen Recording permission; window titles and window images are the gated
/// parts of `CGWindowListCopyWindowInfo`, and neither is touched here.
enum DesktopVisibility {

    /// Coverage is measured by marking cells on a grid rather than computing an
    /// exact rectangle union. The answer feeds a threshold comparison, so ~1%
    /// precision is ample and this stays a few hundred integer operations
    /// against code that would otherwise need a full sweep-line.
    private static let gridColumns = 64
    private static let gridRows = 40

    /// Fraction of `displayID`'s desktop not covered by an opaque window, 0...1.
    /// Returns 1 when the window list can't be read, so a failure here can never
    /// be what stops a wallpaper.
    static func visibleFraction(of displayID: CGDirectDisplayID) -> Double {
        let bounds = CGDisplayBounds(displayID)
        guard bounds.width > 0, bounds.height > 0 else { return 1 }

        guard let windows = CGWindowListCopyWindowInfo(
            [.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
            as? [[String: Any]] else { return 1 }

        let ownPID = ProcessInfo.processInfo.processIdentifier
        var occluders: [CGRect] = []

        for window in windows {
            // Our own desktop window is the thing being occluded, not an
            // occluder. Negative layers are the desktop itself and the icon
            // layer, which sit below or draw over transparently.
            if let pid = window[kCGWindowOwnerPID as String] as? Int32, pid == ownPID { continue }

            // Layer 0 is ordinary application windows, which is exactly what
            // "something is covering my desktop" means. Everything above it is
            // system chrome — the menu bar sits at 24 and the Dock at 20 — and
            // counting those charged a permanent 3.4% of this display against
            // the visible budget for furniture the user doesn't think of as
            // covering anything.
            guard let layer = window[kCGWindowLayer as String] as? Int, layer == 0 else { continue }

            // A translucent window still shows the wallpaper through it. Treat
            // anything not fully opaque as not covering: over-estimating what is
            // visible keeps rendering, which is the safe direction to be wrong.
            let alpha = window[kCGWindowAlpha as String] as? Double ?? 1
            guard alpha >= 0.95 else { continue }

            guard let dict = window[kCGWindowBounds as String] as? [String: Any],
                  let rect = CGRect(dictionaryRepresentation: dict as CFDictionary)
            else { continue }

            occluders.append(rect)
        }

        return uncoveredFraction(of: bounds, occludedBy: occluders)
    }

    /// The geometry, separated from the window-list plumbing so it can be
    /// exercised directly. Rectangles may overlap and may hang off the edge of
    /// `bounds`; both are normal.
    ///
    /// A cell counts as covered when its centre is inside an occluder, so a
    /// window edge cutting through a cell rounds to the side it covers most.
    static func uncoveredFraction(of bounds: CGRect, occludedBy occluders: [CGRect]) -> Double {
        guard bounds.width > 0, bounds.height > 0 else { return 1 }

        var covered = [Bool](repeating: false, count: gridColumns * gridRows)
        let cellWidth = bounds.width / Double(gridColumns)
        let cellHeight = bounds.height / Double(gridRows)

        for rect in occluders {
            let clipped = rect.intersection(bounds)
            guard !clipped.isNull, clipped.width > 0, clipped.height > 0 else { continue }

            for row in 0..<gridRows {
                let centreY = bounds.minY + (Double(row) + 0.5) * cellHeight
                guard centreY >= clipped.minY, centreY < clipped.maxY else { continue }
                for col in 0..<gridColumns {
                    let centreX = bounds.minX + (Double(col) + 0.5) * cellWidth
                    guard centreX >= clipped.minX, centreX < clipped.maxX else { continue }
                    covered[row * gridColumns + col] = true
                }
            }
        }

        let coveredCells = covered.reduce(into: 0) { $0 += $1 ? 1 : 0 }
        return 1 - Double(coveredCells) / Double(gridColumns * gridRows)
    }
}
