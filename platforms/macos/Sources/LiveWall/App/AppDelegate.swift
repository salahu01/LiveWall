import AppKit
import UniformTypeIdentifiers

/// Status-bar-only front end. There is no window, no dock icon and no timer:
/// the menu is rebuilt when it opens, so an idle app does nothing at all.
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {

    private let engine = WallpaperEngine()
    private var statusItem: NSStatusItem!
    private var importing = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.image = StatusIcon.make()
        statusItem.button?.toolTip = "\(Self.appName) \(Self.appVersion)"
        statusItem.button?.setAccessibilityLabel(Self.appName)

        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

        // A rebuild or a move invalidates the launch-at-login registration, and
        // the user's first sign that anything happened would be the app not
        // starting after a restart.
        LoginItem.reconcile()

        engine.onStateChange = { [weak self] in self?.updateButton() }
        engine.start()
        updateButton()
    }

    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool { true }

    // MARK: - Status button

    private func updateButton() {
        guard !importing else { return }
        statusItem.button?.title = ""
    }

    // MARK: - Menu

    /// Read from the bundle rather than hard-coded, so the menu can't drift from
    /// what shipped.
    static var appName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String
            ?? "LiveWall"
    }

    static var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "—"
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        // A status-bar-only app has no title bar and no dock tile, so the menu
        // is the only place its name appears.
        let identity = NSMenuItem(title: "\(Self.appName) \(Self.appVersion)",
                                  action: nil, keyEquivalent: "")
        identity.isEnabled = false
        menu.addItem(identity)

        let status = NSMenuItem(title: engine.statusLine, action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)

        let memory = NSMenuItem(title: "Memory: \(Footprint.formatted())", action: nil, keyEquivalent: "")
        memory.isEnabled = false
        menu.addItem(memory)

        menu.addItem(.separator())

        let shader = NSMenuItem(title: "Procedural (lightest)",
                                action: #selector(selectShader), keyEquivalent: "")
        shader.target = self
        shader.state = engine.library.selectedID == nil ? .on : .off
        menu.addItem(shader)

        if !engine.library.items.isEmpty {
            menu.addItem(.separator())
            for item in engine.library.items {
                let menuItem = NSMenuItem(title: item.title,
                                          action: #selector(selectWallpaper(_:)), keyEquivalent: "")
                menuItem.target = self
                menuItem.representedObject = item.id
                menuItem.state = engine.library.selectedID == item.id ? .on : .off
                menuItem.toolTip = "\(item.resolutionLabel) · \(item.sizeLabel)"
                menu.addItem(menuItem)
            }
        }

        menu.addItem(.separator())

        let add = NSMenuItem(title: importing ? "Converting…" : "Add Video…",
                             action: importing ? nil : #selector(addVideo), keyEquivalent: "o")
        add.target = self
        menu.addItem(add)

        let quality = NSMenuItem(title: "Import Quality", action: nil, keyEquivalent: "")
        let qualityMenu = NSMenu()
        for preset in Transcoder.Preset.all {
            let presetItem = NSMenuItem(title: "\(preset.name) — \(preset.summary)",
                                        action: #selector(selectPreset(_:)), keyEquivalent: "")
            presetItem.target = self
            presetItem.representedObject = preset.name
            presetItem.state = engine.library.preset.name == preset.name ? .on : .off
            qualityMenu.addItem(presetItem)
        }
        quality.submenu = qualityMenu
        menu.addItem(quality)

        menu.addItem(scalingItem())

        let battery = NSMenuItem(title: "Pause on Battery",
                                 action: #selector(togglePauseOnBattery), keyEquivalent: "")
        battery.target = self
        battery.state = engine.library.pauseOnBattery ? .on : .off
        menu.addItem(battery)

        if LoginItem.isSupported {
            let login = NSMenuItem(title: "Open at Login",
                                   action: #selector(toggleLoginItem), keyEquivalent: "")
            login.target = self
            login.state = LoginItem.isEnabled ? .on : .off
            if LoginItem.requiresApproval {
                login.toolTip = "Waiting for approval in System Settings › General › Login Items."
            } else if !LoginItem.isInStableLocation {
                login.toolTip = "LiveWall is running from \(Bundle.main.bundleURL.deletingLastPathComponent().path). "
                    + "Move it to Applications, or this will stop working whenever the app is rebuilt."
            }
            menu.addItem(login)
        }

        if let id = engine.library.selectedID, engine.library.item(withID: id) != nil {
            let remove = NSMenuItem(title: "Remove Current Wallpaper",
                                    action: #selector(removeCurrent), keyEquivalent: "")
            remove.target = self
            menu.addItem(remove)
        }

        let reveal = NSMenuItem(title: "Reveal Library in Finder",
                                action: #selector(revealLibrary), keyEquivalent: "")
        reveal.target = self
        menu.addItem(reveal)

        menu.addItem(.separator())

        let quit = NSMenuItem(title: "Quit LiveWall", action: #selector(quit), keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
    }

    /// Scaling submenu. Each mode's tooltip states what it costs for the
    /// wallpaper actually selected on the main display — the aspect mismatch is
    /// otherwise invisible, and it is the whole reason this menu exists.
    private func scalingItem() -> NSMenuItem {
        let scaling = NSMenuItem(title: "Scaling", action: nil, keyEquivalent: "")
        let submenu = NSMenu()

        let selected = engine.library.selectedID.flatMap { engine.library.item(withID: $0) }
        let content = selected.map { CGSize(width: $0.width, height: $0.height) }
        let display = NSScreen.main?.frame.size

        for mode in FitMode.allCases {
            let item = NSMenuItem(title: "\(mode.title) — \(mode.tradeoff)",
                                  action: #selector(selectFitMode(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = mode.rawValue
            item.state = engine.library.fitMode == mode ? .on : .off
            if let content, let display {
                item.toolTip = mode.effectDescription(content: content, display: display)
                    ?? "The wallpaper and this display have the same shape — every mode looks identical."
            }
            submenu.addItem(item)
        }

        if selected == nil {
            submenu.addItem(.separator())
            let note = NSMenuItem(title: "Applies to video wallpapers", action: nil, keyEquivalent: "")
            note.isEnabled = false
            submenu.addItem(note)
        }

        scaling.submenu = submenu
        return scaling
    }

    // MARK: - Actions

    @objc private func selectFitMode(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String,
              let mode = FitMode(rawValue: raw) else { return }
        engine.setFitMode(mode)
    }

    @objc private func selectShader() {
        engine.select(itemID: nil)
    }

    @objc private func selectWallpaper(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? UUID else { return }
        engine.select(itemID: id)
    }

    @objc private func selectPreset(_ sender: NSMenuItem) {
        guard let name = sender.representedObject as? String,
              let preset = Transcoder.Preset.all.first(where: { $0.name == name }) else { return }
        engine.library.preset = preset
    }

    @objc private func togglePauseOnBattery() {
        engine.setPauseOnBattery(!engine.library.pauseOnBattery)
    }

    @objc private func toggleLoginItem() {
        let wanted = !LoginItem.isEnabled
        LoginItem.setEnabled(wanted)

        // macOS can hold the registration pending the user's approval, in which
        // case the menu would just snap back with no explanation.
        if wanted && LoginItem.requiresApproval {
            let alert = NSAlert()
            alert.messageText = "Approve LiveWall in Login Items"
            alert.informativeText = "macOS needs your approval before LiveWall can start "
                + "automatically. Open System Settings › General › Login Items and enable it there."
            alert.addButton(withTitle: "Open Login Items")
            alert.addButton(withTitle: "Later")
            NSApp.activate(ignoringOtherApps: true)
            if alert.runModal() == .alertFirstButtonReturn,
               let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
                NSWorkspace.shared.open(url)
            }
        }
    }

    @objc private func removeCurrent() {
        guard let id = engine.library.selectedID, let item = engine.library.item(withID: id) else { return }
        engine.library.remove(item)
        engine.select(itemID: nil)
    }

    @objc private func revealLibrary() {
        NSWorkspace.shared.activateFileViewerSelecting([engine.library.directory])
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    @objc private func addVideo() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = [.movie, .video, .mpeg4Movie, .quickTimeMovie]
        panel.message = "Pick a video. It will be converted to a low-power loop; the original is left untouched."
        panel.prompt = "Convert"

        // Frame rate and rotation belong to the clip rather than to the library —
        // a video shot sideways needs a turn the next one will not — so they are
        // asked per import. The panel's accessory view is where that costs the
        // user no extra dialog.
        let accessory = importAccessoryView()
        panel.accessoryView = accessory.view
        panel.isAccessoryViewDisclosed = true

        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let source = panel.url else { return }

        // An empty or unparseable field means "no opinion", which is what
        // `fps == nil` says — the preset's rate then applies.
        let typed = Int(accessory.fpsField.stringValue.trimmingCharacters(in: .whitespaces))
        let options = ImportOptions(
            fps: typed.map(ImportOptions.sanitisedFPS),
            rotationDegrees: ImportOptions.rotations[
                min(max(accessory.rotationPopUp.indexOfSelectedItem, 0),
                    ImportOptions.rotations.count - 1)])

        startImport(of: source, options: options)
    }

    /// The frame-rate field and rotation menu shown inside the open panel.
    ///
    /// Returned as a tuple rather than stored on the delegate because the panel
    /// is modal: the controls live exactly as long as `runModal()` does, and
    /// reading them after it returns is the whole interaction.
    private func importAccessoryView()
        -> (view: NSView, fpsField: NSTextField, rotationPopUp: NSPopUpButton) {

        let preset = engine.library.preset

        let fpsLabel = NSTextField(labelWithString: "Frames per second:")
        let fpsField = NSTextField(string: String(preset.fps))
        fpsField.placeholderString = "\(ImportOptions.minimumFPS)–\(ImportOptions.maximumFPS)"
        fpsField.alignment = .right
        fpsField.widthAnchor.constraint(equalToConstant: 56).isActive = true

        let rotationLabel = NSTextField(labelWithString: "Rotate:")
        let rotationPopUp = NSPopUpButton()
        rotationPopUp.addItems(withTitles: ImportOptions.rotations.map {
            ImportOptions(rotationDegrees: $0).rotationLabel
        })

        let note = NSTextField(wrappingLabelWithString:
            "The source's own rate is the ceiling, and the rate is snapped down "
            + "to divide this display's refresh evenly. Frame rate is the only "
            + "import setting that costs CPU at playback.")
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.textColor = .secondaryLabelColor

        let controls = NSStackView(views: [fpsLabel, fpsField, rotationLabel, rotationPopUp])
        controls.orientation = .horizontal
        controls.spacing = 8

        let column = NSStackView(views: [controls, note])
        column.orientation = .vertical
        column.alignment = .leading
        column.spacing = 6
        column.edgeInsets = NSEdgeInsets(top: 10, left: 20, bottom: 10, right: 20)
        note.widthAnchor.constraint(equalToConstant: 380).isActive = true

        return (column, fpsField, rotationPopUp)
    }

    // MARK: - Import

    private func startImport(of source: URL, options: ImportOptions) {
        importing = true
        let id = UUID()
        let destination = engine.library.destinationURL(id: id)
        let preset = engine.library.preset
        let title = source.deletingPathExtension().lastPathComponent

        statusItem.button?.title = " 0%"

        Task { @MainActor in
            do {
                let result = try await Transcoder.convert(
                    source: source,
                    destination: destination,
                    preset: preset,
                    display: Transcoder.DisplayTarget.main(),
                    options: options,
                    progress: { [weak self] fraction in
                        self?.statusItem.button?.title = String(format: " %d%%", Int(fraction * 100))
                    })

                let item = WallpaperItem(id: id,
                                         title: title,
                                         filename: destination.lastPathComponent,
                                         width: Int(result.size.width),
                                         height: Int(result.size.height),
                                         fps: result.fps,
                                         byteCount: result.byteCount,
                                         addedAt: Date(),
                                         bitDepth: result.bitDepth)
                engine.library.add(item)
                importing = false
                statusItem.button?.title = ""
                engine.select(itemID: id)
            } catch {
                importing = false
                statusItem.button?.title = ""
                try? FileManager.default.removeItem(at: destination)
                presentError(error)
            }
        }
    }

    private func presentError(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = "Couldn't add that video"
        alert.informativeText = error.localizedDescription
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }
}
