import AppKit

/// Ties the pieces together: one `ScreenController` per display, a shared
/// `PowerMonitor`, and the current selection from the `Library`.
///
/// Each display gets its own source instance, so a two-monitor setup decodes
/// twice. Sharing one decoder across displays would be cheaper, but only when
/// both are visible — and the common case is that one of them is covered, where
/// separate sources let the covered one tear down independently. Worth
/// revisiting if three-plus display setups turn out to be common.
final class WallpaperEngine {

    let library = Library()
    let power = PowerMonitor()

    private var controllers: [CGDirectDisplayID: ScreenController] = [:]
    private var pendingPrepares = 0

    /// Bumped on every selection change so that an asset load finishing late
    /// can tell whether it is still the selection anyone wants.
    private var generation: UInt64 = 0

    /// Called when something the status menu displays has changed.
    var onStateChange: (() -> Void)?

    func start() {
        power.pauseOnBattery = library.pauseOnBattery
        power.onChange = { [weak self] in
            guard let self else { return }
            self.controllers.values.forEach { $0.evaluate() }
            self.onStateChange?()
        }
        power.start()

        NotificationCenter.default.addObserver(
            self, selector: #selector(screensChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)

        syncScreens()
        applySelection()
    }

    // MARK: - Screens

    /// `didChangeScreenParameters` fires for far more than displays being
    /// plugged in — resolution changes, wake, menu bar changes, Stage Manager.
    /// Rebuilding every source on each one meant tearing down and recreating a
    /// decoder several times a minute, which both churned CPU and grew the
    /// process's footprint steadily. Existing screens only need their geometry
    /// updated; only genuinely new screens need a source built.
    @objc private func screensChanged() {
        let added = syncScreens()
        if !added.isEmpty { applySelection(to: added) }
        onStateChange?()
    }

    @discardableResult
    private func syncScreens() -> [ScreenController] {
        var seen = Set<CGDirectDisplayID>()
        var added: [ScreenController] = []

        for screen in NSScreen.screens {
            let id = ScreenController.displayID(of: screen)
            seen.insert(id)
            if let existing = controllers[id] {
                existing.updateScreen(screen)
            } else {
                let controller = ScreenController(screen: screen, power: power)
                controller.onStateChange = { [weak self] in self?.onStateChange?() }
                controllers[id] = controller
                added.append(controller)
            }
        }

        for (id, controller) in controllers where !seen.contains(id) {
            controller.setSource(nil)
            controllers.removeValue(forKey: id)
        }

        return added
    }

    // MARK: - Selection

    func select(itemID: UUID?) {
        library.selectedID = itemID
        applySelection()
        onStateChange?()
    }

    func applySelection() {
        applySelection(to: Array(controllers.values))
    }

    private func applySelection(to targets: [ScreenController]) {
        guard !targets.isEmpty else { return }
        // Any in-flight asset load from a previous selection is now stale.
        generation &+= 1
        guard let id = library.selectedID, let item = library.item(withID: id) else {
            useShader(on: targets)
            return
        }
        useVideo(item, on: targets)
    }

    /// The procedural mode. Prefers the Core Animation gradient — it needs no
    /// Metal device at all and the compositor does the interpolation — and only
    /// uses the Metal shader when a precompiled metallib was bundled. Compiling
    /// that shader at runtime instead would cost ~97 MB, which is more than the
    /// rest of the app combined and defeats the point of the mode.
    static func makeProceduralSource() -> WallpaperSource {
        if Bundle.main.url(forResource: "wallpaper", withExtension: "metallib") != nil {
            return ShaderSource(fps: 15)
        }
        return GradientSource()
    }

    private func useShader(on targets: [ScreenController]) {
        for controller in targets {
            controller.setSource(Self.makeProceduralSource())
        }
        onStateChange?()
    }

    private func useVideo(_ item: WallpaperItem, on targets: [ScreenController]) {
        let url = library.url(for: item)
        guard FileManager.default.fileExists(atPath: url.path) else {
            Log.error("selected wallpaper missing on disk, falling back to procedural")
            library.selectedID = nil
            useShader(on: targets)
            return
        }

        let token = generation
        let fitMode = library.fitMode
        for controller in targets {
            let source = VideoSource(url: url, fps: item.fps,
                                     bitDepth: item.pixelBitDepth, fitMode: fitMode)
            pendingPrepares += 1
            source.prepare { [weak self, weak controller] ok in
                guard let self else { return }
                self.pendingPrepares -= 1

                // The selection changed while the asset was loading; this
                // source would otherwise install itself over the newer one.
                guard token == self.generation else { return }

                guard ok else {
                    Log.error("could not prepare \(item.title); falling back to procedural")
                    controller?.setSource(Self.makeProceduralSource())
                    self.onStateChange?()
                    return
                }
                controller?.setSource(source)
                self.onStateChange?()
            }
        }
    }

    // MARK: - Status

    var isAnyRendering: Bool {
        controllers.values.contains { $0.isRendering }
    }

    var screenCount: Int { controllers.count }

    /// One-line summary for the top of the status menu.
    var statusLine: String {
        if let reason = power.blockReason() {
            return "Paused — \(reason)"
        }
        if pendingPrepares > 0 { return "Loading…" }
        if controllers.isEmpty { return "No displays" }
        if isAnyRendering { return "Rendering" }
        let nearlyCovered = controllers.values.contains { $0.isNearlyCovered }
        return nearlyCovered ? "Paused — desktop nearly covered" : "Paused — desktop covered"
    }

    /// Applied to live sources in place — no reload, no decode interruption.
    func setFitMode(_ mode: FitMode) {
        library.fitMode = mode
        controllers.values.forEach { $0.setFitMode(mode) }
        onStateChange?()
    }

    func setPauseOnBattery(_ value: Bool) {
        library.pauseOnBattery = value
        power.pauseOnBattery = value
        onStateChange?()
    }
}
