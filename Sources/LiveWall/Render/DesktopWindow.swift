import AppKit

/// A borderless, click-through window pinned behind the desktop icons.
///
/// The level is `kCGDesktopWindowLevel`, which sits below
/// `kCGDesktopIconWindowLevel` — so icons, Stage Manager and everything else
/// draw on top and the window is never a target for clicks or window cycling.
final class DesktopWindow: NSWindow {

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }

    init(screen: NSScreen) {
        super.init(contentRect: screen.frame,
                   styleMask: [.borderless],
                   backing: .buffered,
                   defer: false)

        level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.desktopWindow)))
        collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenNone]

        isOpaque = false
        hasShadow = false
        backgroundColor = .clear
        ignoresMouseEvents = true
        isMovable = false
        isReleasedWhenClosed = false
        // Nothing here belongs in a window restoration snapshot.
        isRestorable = false
        animationBehavior = .none

        let view = NSView(frame: NSRect(origin: .zero, size: screen.frame.size))
        view.wantsLayer = true
        // Deliberately transparent, never black.
        //
        // This window sits above the system desktop picture, so anything it
        // paints hides it. Black looked harmless while a video was always
        // playing, but the moment rendering is gated off before a single frame
        // is decoded — a covered desktop at launch, a Space we haven't drawn on
        // — an opaque black rectangle is all there is, and it covers the user's
        // real wallpaper. Clear means "no wallpaper of ours" degrades to the
        // system's own instead of to black.
        view.layer?.backgroundColor = NSColor.clear.cgColor
        contentView = view

        setFrame(screen.frame, display: false)
    }

    func install(layer: CALayer) {
        guard let host = contentView?.layer else { return }
        host.sublayers?.forEach { $0.removeFromSuperlayer() }
        layer.frame = host.bounds
        host.addSublayer(layer)
    }
}
