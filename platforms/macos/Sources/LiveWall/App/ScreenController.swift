import AppKit

/// Owns one screen's desktop window and the source drawing into it, and decides
/// moment to moment whether that source should be running at all.
///
/// Rendering is allowed only when both hold:
///   - the window reports `.visible` occlusion (nothing opaque is covering the
///     desktop on this display), and
///   - no machine-wide condition blocks it (lock, sleep, Low Power Mode…).
///
/// Deactivation is delayed slightly so that transient occlusion during window
/// animations or Mission Control doesn't tear down and rebuild the decoder
/// several times a second. Activation is immediate — there is no reason to make
/// the user wait to see their wallpaper.
final class ScreenController {

    private static let deactivateDelay: TimeInterval = 0.4

    /// Stop once this little of the desktop is left uncovered, resume once this
    /// much is back. The gap between the two is deliberate: a single threshold
    /// makes a window edge resting near it toggle the decoder repeatedly.
    private static let stopBelowFraction = 0.08
    private static let resumeAboveFraction = 0.15

    /// Windows can be moved and resized without any notification we can observe,
    /// so the uncovered fraction has to be sampled.
    ///
    /// Profiling put this poll at 11 main-thread samples against 7 for the
    /// entire frame pump over the same six seconds — the gate was costing more
    /// than the video it was gating. Walking the window list isn't free, so it
    /// happens rarely: a wallpaper that takes four seconds to notice it has been
    /// covered has cost nothing anyone can perceive.
    private static let visibilityPollInterval: TimeInterval = 4

    /// Keeps rendering even when the desktop is covered. Exists so playback cost
    /// can be measured on a machine whose desktop is behind a terminal — the
    /// occlusion gate otherwise tears the decoder down and every reading comes
    /// back as the idle baseline. Never set in normal use.
    private static let forceRender = ProcessInfo.processInfo.environment["LIVEWALL_FORCE_RENDER"] != nil

    let displayID: CGDirectDisplayID
    private(set) var screen: NSScreen
    private let window: DesktopWindow
    private var source: WallpaperSource?
    private unowned let power: PowerMonitor

    private var pendingDeactivate: DispatchWorkItem?

    /// Latched through the hysteresis band rather than recomputed as a plain
    /// comparison each time.
    private var desktopNearlyCovered = false
    private var visibilityTimer: Timer?

    /// Fired when this screen's own gating changed, so the status menu can
    /// reflect a stop that no system notification announced.
    var onStateChange: (() -> Void)?

    var isRendering: Bool { source?.isActive ?? false }
    var sourceSummary: String { source?.summary ?? "none" }

    /// True when rendering stopped because there was almost no desktop left to
    /// see, as opposed to none at all.
    var isNearlyCovered: Bool { desktopNearlyCovered }

    init(screen: NSScreen, power: PowerMonitor) {
        self.screen = screen
        self.power = power
        self.displayID = Self.displayID(of: screen)
        self.window = DesktopWindow(screen: screen)

        NotificationCenter.default.addObserver(
            self, selector: #selector(occlusionChanged),
            name: NSWindow.didChangeOcclusionStateNotification, object: window)

        window.orderFrontRegardless()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
        pendingDeactivate?.cancel()
        visibilityTimer?.invalidate()
        source?.deactivate()
        window.orderOut(nil)
    }

    // MARK: - Source

    func setSource(_ newSource: WallpaperSource?) {
        pendingDeactivate?.cancel()
        source?.deactivate()
        source = newSource

        guard let newSource else {
            stopVisibilityPolling()
            return
        }
        newSource.attach(to: screen)
        newSource.updateGeometry(size: screen.frame.size, scale: screen.backingScaleFactor)
        window.install(layer: newSource.layer)
        startVisibilityPolling()
        evaluate()
    }

    // MARK: - Visible desktop area

    private func startVisibilityPolling() {
        guard visibilityTimer == nil else { return }
        let timer = Timer(timeInterval: Self.visibilityPollInterval, repeats: true) { [weak self] _ in
            self?.evaluate()
        }
        // Tolerance lets the system coalesce this with other timers instead of
        // waking the CPU on its own account.
        timer.tolerance = Self.visibilityPollInterval / 2
        RunLoop.main.add(timer, forMode: .common)
        visibilityTimer = timer
    }

    private func stopVisibilityPolling() {
        visibilityTimer?.invalidate()
        visibilityTimer = nil
    }

    /// Moves the latch across the hysteresis band. Reports whether it changed,
    /// so the caller can tell the menu something it didn't already know.
    @discardableResult
    private func updateCoverageLatch() -> Bool {
        let fraction = DesktopVisibility.visibleFraction(of: displayID)
        let wasNearlyCovered = desktopNearlyCovered

        if desktopNearlyCovered {
            if fraction >= Self.resumeAboveFraction { desktopNearlyCovered = false }
        } else if fraction < Self.stopBelowFraction {
            desktopNearlyCovered = true
        }

        if desktopNearlyCovered != wasNearlyCovered {
            Log.info(String(format: "desktop %.0f%% visible — %@",
                            fraction * 100,
                            desktopNearlyCovered ? "stopping" : "resuming"))
            return true
        }
        return false
    }

    func setFitMode(_ mode: FitMode) {
        source?.setFitMode(mode)
    }

    func updateScreen(_ screen: NSScreen) {
        self.screen = screen
        window.setFrame(screen.frame, display: false)
        window.contentView?.frame = CGRect(origin: .zero, size: screen.frame.size)
        window.contentView?.layer?.frame = CGRect(origin: .zero, size: screen.frame.size)
        source?.attach(to: screen)
        source?.updateGeometry(size: screen.frame.size, scale: screen.backingScaleFactor)
        evaluate()
    }

    // MARK: - Activation

    @objc private func occlusionChanged() {
        evaluate()
    }

    func evaluate() {
        guard let source else { return }

        // Occlusion is the cheap signal and fires on its own; the uncovered
        // fraction is the expensive one — it walks the window list — but it
        // catches what occlusion cannot see, since a window covering all but a
        // corner still reports "visible".
        //
        // So only pay for it when it can change the answer. If the desktop is
        // fully occluded, or the machine is blocking anyway, we are already
        // stopped and a notification will wake us when that changes.
        let occlusionVisible = window.occlusionState.contains(.visible)
        let systemBlocked = power.systemBlocksRendering
        let latchChanged = (occlusionVisible && !systemBlocked) ? updateCoverageLatch() : false

        let visible = occlusionVisible && !desktopNearlyCovered
        let shouldRender = (visible && !systemBlocked) || Self.forceRender

        if latchChanged { onStateChange?() }

        if shouldRender {
            pendingDeactivate?.cancel()
            pendingDeactivate = nil
            if !source.isActive { source.activate() }
        } else if source.isActive, pendingDeactivate == nil {
            let work = DispatchWorkItem { [weak self] in
                guard let self, let source = self.source else { return }
                self.pendingDeactivate = nil
                let stillBlocked = !self.window.occlusionState.contains(.visible)
                    || self.power.systemBlocksRendering
                if stillBlocked { source.deactivate() }
            }
            pendingDeactivate = work
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.deactivateDelay, execute: work)
        }
    }

    // MARK: - Helpers

    static func displayID(of screen: NSScreen) -> CGDirectDisplayID {
        let key = NSDeviceDescriptionKey("NSScreenNumber")
        return (screen.deviceDescription[key] as? NSNumber)?.uint32Value ?? 0
    }
}
