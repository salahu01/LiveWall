// Start with the session — the LoginItem equivalent.
//
// A .desktop file in $XDG_CONFIG_HOME/autostart, which every desktop
// environment reads and which needs no daemon, no elevation and no service
// manager. Not a systemd user unit by default: the unit is shipped in
// packaging/ for anyone who wants it, but `systemctl --user enable` on a
// machine whose session is not systemd-managed appears to work and then never
// starts anything, which is precisely the failure mode the macOS port's
// SMAppService section spends four paragraphs on.
//
// One thing works better here than on either of the other two platforms, and it
// is worth saying because both of them have a caveat in this spot. The autostart
// entry stores a command line in a text file the user can read. macOS binds a
// registration to the exact bundle LaunchServices saw, so a rebuild silently
// breaks it; Windows stores a path in the registry, which breaks visibly when
// the exe moves. Here the breakage is visible, repairable by hand, and
// reconciled automatically on the next launch by `reconcile()`.
#pragma once

namespace livewall {

class StartupItem {
public:
    static bool isEnabled();
    static bool setEnabled(bool enabled);

    // If the entry exists but names a different executable — the usual cause is
    // the user moving or reinstalling the binary — it is rewritten to point
    // here. Called once at launch.
    static void reconcile();

    // True when the current entry names this executable. False both when there
    // is no entry and when it names a different path.
    static bool pointsAtThisExecutable();
};

}  // namespace livewall
