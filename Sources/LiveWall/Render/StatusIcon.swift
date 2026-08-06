import AppKit

/// The menu bar icon.
///
/// For a `LSUIElement` app this is the only mark most users ever see — there is
/// no dock tile and no window — so it is drawn rather than borrowed from SF
/// Symbols, and it echoes the app icon: a rounded display with a wave running
/// through it.
///
/// Drawn at request time instead of shipped as a PNG so it stays sharp on any
/// scale factor, and marked as a template so macOS handles light mode, dark
/// mode, menu bar tinting and the highlighted state itself.
enum StatusIcon {

    /// Menu bar images are sized in points; 18 is the conventional height that
    /// leaves the standard optical margin above and below.
    static func make(height: CGFloat = 18) -> NSImage {
        let size = NSSize(width: height + 2, height: height)

        let image = NSImage(size: size, flipped: false) { rect in
            let inset: CGFloat = 1.5
            let body = rect.insetBy(dx: inset, dy: inset + 0.5)
            let radius = body.height * 0.28

            let outline = NSBezierPath(roundedRect: body, xRadius: radius, yRadius: radius)
            outline.lineWidth = 1.4
            NSColor.black.setStroke()
            outline.stroke()

            // The wave, clipped to the display shape so it reads as content
            // inside a screen rather than a line crossing it.
            NSGraphicsContext.saveGraphicsState()
            outline.addClip()

            let wave = NSBezierPath()
            let midline = body.midY - body.height * 0.04
            let amplitude = body.height * 0.17
            var x = body.minX
            wave.move(to: NSPoint(x: x, y: midline))
            while x <= body.maxX {
                let t = (x - body.minX) / body.width
                wave.line(to: NSPoint(x: x, y: midline + sin(t * .pi * 2 - .pi / 2) * amplitude))
                x += 0.5
            }
            // Closed down to the bottom edge and filled: a stroked hairline
            // disappears at menu bar sizes, a filled mass does not.
            wave.line(to: NSPoint(x: body.maxX, y: body.minY))
            wave.line(to: NSPoint(x: body.minX, y: body.minY))
            wave.close()
            NSColor.black.withAlphaComponent(0.55).setFill()
            wave.fill()

            NSGraphicsContext.restoreGraphicsState()
            return true
        }

        // Template images are recoloured by AppKit for the current menu bar
        // appearance; without this the icon stays black on a dark menu bar.
        image.isTemplate = true
        return image
    }
}
