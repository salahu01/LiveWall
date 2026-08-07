import Foundation
import ServiceManagement

/// Registers the app to start at login.
///
/// `SMAppService.mainApp` is the modern replacement for the deprecated
/// `SMLoginItemSetEnabled` and for dropping a plist into `LaunchAgents`: macOS
/// owns the registration, the user can see and revoke it in System Settings >
/// General > Login Items, and there is no helper bundle to keep signed.
///
/// The system is the source of truth for whether the item is *on*: if we trusted
/// a flag of our own it would disagree with reality the moment the user switched
/// it off in System Settings.
///
/// **The app has to be installed to register at all.** Run out of a build
/// directory, `SMAppService.mainApp.status` is `.notFound` and `register()` has
/// nothing to bind to — LaunchServices does not consider the bundle an installed
/// application. The toggle appears to work, nothing is recorded, and the user
/// discovers it after a restart. `isInStableLocation` is what the menu warns on;
/// `tools/install.sh` is the fix.
///
/// Replacing an installed bundle in place is fine — measured: deleting
/// `/Applications/LiveWall.app` outright and copying a fresh build back left the
/// registration `.enabled`. `reconcile()` covers the case where it doesn't
/// anyway, since the failure is silent and the cost of checking is nil.
enum LoginItem {

    private static let intentKey = "openAtLogin"

    /// Unavailable when running the bare executable rather than the .app —
    /// registration is per-bundle, so there is nothing to register.
    static var isSupported: Bool {
        Bundle.main.bundleIdentifier != nil && Bundle.main.bundleURL.pathExtension == "app"
    }

    /// A login item has to point at a bundle that will still be there next boot.
    /// An app run out of a build directory is not that: `bundle.sh` deletes and
    /// recreates it on every build, which invalidates the registration.
    static var isInStableLocation: Bool {
        let path = Bundle.main.bundleURL.path
        return path.hasPrefix("/Applications/")
            || path.hasPrefix(NSHomeDirectory() + "/Applications/")
    }

    static var isEnabled: Bool {
        guard isSupported else { return false }
        return SMAppService.mainApp.status == .enabled
    }

    /// Re-registers when macOS has lost a registration the user asked for.
    ///
    /// Deliberately narrow: only `.notFound` is repaired, because that is the
    /// status for "there is no such service" rather than "the user turned it
    /// off" — a user who switches the item off in System Settings leaves
    /// `.requiresApproval`, and re-registering there would be us overriding
    /// them.
    ///
    /// Untested in anger: no way was found to make an *installed* copy report
    /// `.notFound`. It exists because the failure it guards against is silent.
    static func reconcile() {
        guard isSupported, UserDefaults.standard.bool(forKey: intentKey) else { return }
        guard SMAppService.mainApp.status == .notFound else { return }

        Log.error("launch-at-login registration was lost — the app bundle was replaced "
                  + "or moved; re-registering")
        setEnabled(true)
    }

    /// Returns whether the change took effect.
    ///
    /// Failure here is normal rather than exceptional — an unsigned build, or a
    /// user who has denied the app in System Settings — so it reports back
    /// instead of throwing, and the caller re-reads the real state either way.
    @discardableResult
    static func setEnabled(_ enabled: Bool) -> Bool {
        guard isSupported else { return false }
        UserDefaults.standard.set(enabled, forKey: intentKey)
        do {
            if enabled {
                // Registering something already registered throws rather than
                // being a no-op.
                guard SMAppService.mainApp.status != .enabled else { return true }
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            return true
        } catch {
            Log.error("could not \(enabled ? "enable" : "disable") launch at login: \(error)")
            return false
        }
    }

    /// Why the toggle is off when the user just switched it on — the one case
    /// worth explaining rather than silently reverting.
    static var requiresApproval: Bool {
        isSupported && SMAppService.mainApp.status == .requiresApproval
    }
}
