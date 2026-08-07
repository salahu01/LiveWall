#!/bin/bash
# Builds LiveWall and installs it to /Applications.
#
#   ./tools/install.sh
#   PREFIX=~/Applications ./tools/install.sh
#
# Why this exists rather than just running out of build/:
#
# A launch-at-login registration is bound to the exact bundle macOS saw when it
# was made. `bundle.sh` deletes and recreates build/LiveWall.app on every build,
# so a registration pointing there goes to `notFound` the next time you build —
# the switch turns itself off and the app stops starting at login. The same is
# true of anything else keyed to app identity. An installed copy is stable;
# build/ is not.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-/Applications}"
SOURCE="$ROOT/build/LiveWall.app"
TARGET="$PREFIX/LiveWall.app"

"$ROOT/tools/bundle.sh" "${1:-release}"

if [ ! -d "$SOURCE" ]; then
    echo "no bundle at $SOURCE" >&2
    exit 1
fi

if [ ! -w "$PREFIX" ]; then
    echo "$PREFIX is not writable. Try: PREFIX=~/Applications $0" >&2
    exit 1
fi

# Replacing a running app leaves the old process attached to a deleted bundle,
# which is exactly the state that breaks the login item.
if pgrep -f "$TARGET/Contents/MacOS/LiveWall" >/dev/null 2>&1; then
    echo "==> Quitting the running copy"
    osascript -e 'quit app id "com.fegno.livewall"' 2>/dev/null || true
    for _ in $(seq 1 20); do
        pgrep -f "$TARGET/Contents/MacOS/LiveWall" >/dev/null 2>&1 || break
        sleep 0.25
    done
    pkill -f "$TARGET/Contents/MacOS/LiveWall" 2>/dev/null || true
fi

echo "==> Installing to $TARGET"
rm -rf "$TARGET"
cp -R "$SOURCE" "$TARGET"

# LaunchServices otherwise keeps serving the old bundle record, so `open` and the
# login item can disagree about which copy is the real one.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f "$TARGET" 2>/dev/null || true

echo "==> Launching"
open "$TARGET"

echo
echo "Installed: $TARGET"
echo "Enable 'Open at Login' from the status menu — it will now survive a rebuild,"
echo "because this copy is not the one bundle.sh overwrites."
