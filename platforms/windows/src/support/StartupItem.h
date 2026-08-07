// Start with Windows — the LoginItem equivalent.
//
// Registered under HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Not the
// Startup folder (a shortcut there is invisible to the Task Manager's Startup
// tab in older builds, and users delete it by accident), not a scheduled task
// (which needs elevation to create), and not HKLM (which needs elevation and
// would enable the app for every account on the machine).
//
// One thing works better here than it does on macOS, and it is worth saying
// because the macOS README spends four paragraphs on the opposite. `SMAppService`
// binds a registration to the exact app bundle LaunchServices saw, so a rebuild
// or a move silently breaks it. The Run key stores a command line. Moving the
// executable still breaks it — the stored path no longer exists — but the
// breakage is visible, repairable and reconciled automatically on the next
// launch by `reconcile()`.
#pragma once

namespace livewall {

class StartupItem {
public:
    static bool isEnabled();
    static bool setEnabled(bool enabled);

    // If the app is registered but the stored command line points somewhere
    // else — the usual cause is the user moving or reinstalling the exe — the
    // registration is rewritten to point here. Called once at launch.
    //
    // The alternative is what macOS does: nothing, and the user finds out after
    // a restart that the app no longer starts.
    static void reconcile();

    // True when the current registration names this executable. False both when
    // there is no registration and when it names a different path.
    static bool pointsAtThisExecutable();
};

}  // namespace livewall
