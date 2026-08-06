import Foundation
import AppKit
import CoreGraphics
import IOKit.ps

/// Watches every system condition that should stop wallpaper rendering.
///
/// Occlusion and the uncovered-desktop fraction are handled per-window by
/// `ScreenController`; this covers the machine-wide signals: screen lock,
/// display sleep, screensaver, Low Power Mode, thermal pressure, a nearly flat
/// battery, an absent user, and (optionally) running on battery at all.
///
/// Every one of these is a hard stop rather than a slowdown. That is forced by
/// the decode path: playback pulls one frame per display-link tick and the
/// assets have no B-frames, so every frame is a reference frame that must be
/// decoded whether or not it is shown. Lowering the tick rate would play the
/// clip in slow motion, not more cheaply. Stopping, on the other hand, is free
/// *and* graceful — the last frame stays composited by WindowServer, so a
/// stopped wallpaper looks like a still rather than a black rectangle.
final class PowerMonitor {
    /// Called whenever the answer to "should we render at all?" may have changed.
    var onChange: (() -> Void)?

    /// User setting: suspend rendering whenever the machine is on battery.
    var pauseOnBattery = false {
        didSet { onChange?() }
    }

    private(set) var screensAsleep = false
    private(set) var screenLocked = false
    private(set) var screensaverRunning = false
    private(set) var userIsAway = false

    private var runLoopSource: CFRunLoopSource?
    private var heartbeat: Timer?

    /// No input for this long means nobody is looking at the desktop.
    ///
    /// Deliberately longer than it needs to be to catch a real absence: display
    /// sleep and the screensaver already handle those, so the only thing this
    /// adds is the case where both are disabled. Five minutes was tried and it
    /// froze the wallpaper while the user was sitting there reading, which reads
    /// as a bug rather than a saving.
    private static let awayAfter: TimeInterval = 15 * 60

    /// Below this, on battery, the wallpaper stops regardless of the
    /// pause-on-battery setting. Ambient decoration is not what the last of a
    /// charge is for.
    private static let lowBatteryFraction = 0.20

    /// `.serious` already means fans up and other work being throttled; a
    /// wallpaper has no business competing at that point.
    private var thermallyPressured: Bool {
        switch ProcessInfo.processInfo.thermalState {
        case .serious, .critical: return true
        default: return false
        }
    }

    /// True when nothing on this machine should be rendering, regardless of
    /// what any individual window's occlusion state says.
    var systemBlocksRendering: Bool { blockReason() != nil }

    var isOnBattery: Bool { cachedPower?.onBattery ?? false }

    /// Remaining charge as 0...1, or nil on a machine with no battery.
    var batteryFraction: Double? { cachedPower?.fraction }

    /// `blockReason()` is consulted every time any screen re-evaluates, which is
    /// often enough that copying the IOKit power-source dictionaries each time
    /// would be its own small waste. Charge moves slowly; refreshing it on the
    /// power-source notification and on the heartbeat is plenty.
    private var cachedPower: (onBattery: Bool, fraction: Double?)?

    private func refreshPowerSource() {
        cachedPower = readPowerSource()
    }

    private func readPowerSource() -> (onBattery: Bool, fraction: Double?)? {
        guard let snapshot = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(snapshot)?.takeRetainedValue() as? [CFTypeRef]
        else { return nil }

        for source in sources {
            guard let desc = IOPSGetPowerSourceDescription(snapshot, source)?.takeUnretainedValue()
                    as? [String: Any],
                  let state = desc[kIOPSPowerSourceStateKey] as? String else { continue }

            var fraction: Double?
            if let current = desc[kIOPSCurrentCapacityKey] as? Double,
               let max = desc[kIOPSMaxCapacityKey] as? Double, max > 0 {
                fraction = current / max
            }
            return (state == kIOPSBatteryPowerValue, fraction)
        }
        return nil
    }

    /// Seconds since the user last touched anything. `.combinedSessionState`
    /// covers keyboard, mouse and trackpad without needing any of the input
    /// permissions an event tap would.
    private var secondsSinceInput: TimeInterval {
        CGEventSource.secondsSinceLastEventType(.combinedSessionState,
                                                eventType: .init(rawValue: ~0)!)
    }

    func start() {
        let workspace = NSWorkspace.shared.notificationCenter
        workspace.addObserver(self, selector: #selector(screensSlept),
                              name: NSWorkspace.screensDidSleepNotification, object: nil)
        workspace.addObserver(self, selector: #selector(screensWoke),
                              name: NSWorkspace.screensDidWakeNotification, object: nil)

        let distributed = DistributedNotificationCenter.default()
        distributed.addObserver(self, selector: #selector(screenLockedNotification),
                                name: NSNotification.Name("com.apple.screenIsLocked"), object: nil)
        distributed.addObserver(self, selector: #selector(screenUnlockedNotification),
                                name: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil)
        distributed.addObserver(self, selector: #selector(screensaverStarted),
                                name: NSNotification.Name("com.apple.screensaver.didstart"), object: nil)
        distributed.addObserver(self, selector: #selector(screensaverStopped),
                                name: NSNotification.Name("com.apple.screensaver.didstop"), object: nil)

        NotificationCenter.default.addObserver(
            self, selector: #selector(powerStateChanged),
            name: .NSProcessInfoPowerStateDidChange, object: nil)
        NotificationCenter.default.addObserver(
            self, selector: #selector(powerStateChanged),
            name: ProcessInfo.thermalStateDidChangeNotification, object: nil)

        refreshPowerSource()
        startPowerSourceWatch()
        scheduleHeartbeat()
    }

    /// Idle time and charge level have no notifications, so they get polled.
    ///
    /// The interval is asymmetric on purpose. Noticing that the user *left* can
    /// be lazy — nothing is wrong with rendering fifteen seconds longer than
    /// strictly needed. Noticing they came *back* has to be quick, or the
    /// wallpaper visibly sits frozen after the first keypress.
    private func scheduleHeartbeat() {
        heartbeat?.invalidate()
        let interval: TimeInterval = userIsAway ? 1 : 15
        let timer = Timer(timeInterval: interval, repeats: true) { [weak self] _ in
            self?.pollPolledSignals()
        }
        timer.tolerance = interval / 3
        RunLoop.main.add(timer, forMode: .common)
        heartbeat = timer
    }

    private func pollPolledSignals() {
        let previousCharge = cachedPower
        refreshPowerSource()
        if previousCharge?.onBattery != cachedPower?.onBattery { onChange?() }

        let away = secondsSinceInput >= Self.awayAfter
        guard away != userIsAway else { return }
        userIsAway = away
        Log.info(away ? "user away — stopping" : "user back — resuming")
        // The polling rate depends on which side of this we're on.
        scheduleHeartbeat()
        onChange?()
    }

    func stop() {
        heartbeat?.invalidate()
        heartbeat = nil
    }

    /// IOKit callback fires on AC/battery transitions so `pauseOnBattery` reacts
    /// immediately instead of on the next unrelated notification.
    private func startPowerSourceWatch() {
        let context = Unmanaged.passUnretained(self).toOpaque()
        guard let source = IOPSNotificationCreateRunLoopSource({ ctx in
            guard let ctx else { return }
            let monitor = Unmanaged<PowerMonitor>.fromOpaque(ctx).takeUnretainedValue()
            DispatchQueue.main.async {
                monitor.refreshPowerSource()
                monitor.onChange?()
            }
        }, context)?.takeRetainedValue() else { return }

        runLoopSource = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .defaultMode)
    }

    @objc private func screensSlept() { screensAsleep = true; onChange?() }
    @objc private func screensWoke() { screensAsleep = false; onChange?() }
    @objc private func screenLockedNotification() { screenLocked = true; onChange?() }
    @objc private func screenUnlockedNotification() { screenLocked = false; onChange?() }
    @objc private func screensaverStarted() { screensaverRunning = true; onChange?() }
    @objc private func screensaverStopped() { screensaverRunning = false; onChange?() }
    @objc private func powerStateChanged() { onChange?() }

    /// Human-readable reason rendering is suspended, for the status menu. This
    /// is also the definition of `systemBlocksRendering` — one list, so the menu
    /// can never claim a reason the gate doesn't actually apply.
    func blockReason() -> String? {
        if screensAsleep { return "display asleep" }
        if screenLocked { return "screen locked" }
        if screensaverRunning { return "screensaver" }
        if userIsAway { return "no one here" }
        if thermallyPressured { return "machine running hot" }
        if ProcessInfo.processInfo.isLowPowerModeEnabled { return "Low Power Mode" }
        if let source = cachedPower, source.onBattery {
            if pauseOnBattery { return "on battery" }
            if let fraction = source.fraction, fraction < Self.lowBatteryFraction {
                return "battery low"
            }
        }
        return nil
    }
}
