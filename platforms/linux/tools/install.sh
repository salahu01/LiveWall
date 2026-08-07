#!/usr/bin/env bash
#
# Builds and installs LiveWall, then starts it.
#
#   ./tools/install.sh                  ~/.local  (no root needed)
#   PREFIX=/usr/local sudo ./tools/install.sh
#
# Installs to a prefix rather than a package because there is no package: the
# binary is ~470 KB and depends only on things a desktop machine already has.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BUILD_DIR="${BUILD_DIR:-out/build/default}"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$SOURCE_DIR"

missing=()
for tool in cmake ninja pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing build tools: ${missing[*]}" >&2
    echo >&2
    echo "  Debian/Ubuntu  sudo apt install build-essential cmake ninja-build pkg-config \\" >&2
    echo "                   libx11-dev libxext-dev libxrandr-dev libxfixes-dev \\" >&2
    echo "                   libegl-dev libgles-dev libwayland-dev wayland-protocols \\" >&2
    echo "                   libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \\" >&2
    echo "                   libva-dev libdbus-1-dev" >&2
    echo "  Fedora         sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \\" >&2
    echo "                   libX11-devel libXext-devel libXrandr-devel libXfixes-devel \\" >&2
    echo "                   mesa-libEGL-devel mesa-libGLES-devel wayland-devel \\" >&2
    echo "                   ffmpeg-free-devel libva-devel dbus-devel" >&2
    echo "  Arch           sudo pacman -S base-devel cmake ninja libx11 libxrandr libxfixes \\" >&2
    echo "                   mesa wayland ffmpeg libva dbus" >&2
    exit 1
fi

echo "Configuring…"
cmake --preset default >/dev/null

echo "Building…"
cmake --build --preset default

echo "Testing…"
ctest --preset default >/dev/null

# Stop a running copy first. Installing over a running binary works on Linux —
# the inode stays alive — but the old process keeps the control socket, so a
# newly installed version would refuse to start until it was stopped anyway.
if "$BUILD_DIR/livewall" status >/dev/null 2>&1; then
    echo "Stopping the running copy…"
    "$BUILD_DIR/livewall" quit >/dev/null 2>&1 || true
    sleep 1
fi

echo "Installing to $PREFIX…"
install -Dm755 "$BUILD_DIR/livewall" "$PREFIX/bin/livewall"
install -Dm644 packaging/livewall.desktop "$PREFIX/share/applications/livewall.desktop"
install -Dm644 ../../docs/icon.png "$PREFIX/share/icons/hicolor/256x256/apps/livewall.png"

# So the tray host can find the icon this run rather than after the next login.
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
fi

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *)
        echo
        echo "Note: $PREFIX/bin is not on your PATH." >&2
        echo "      Add it, or run $PREFIX/bin/livewall directly." >&2
        ;;
esac

echo
echo "Starting…"
"$PREFIX/bin/livewall" >/dev/null 2>&1 &
sleep 2

if "$PREFIX/bin/livewall" status >/dev/null 2>&1; then
    "$PREFIX/bin/livewall" status
    echo
    echo "Add a video with:  livewall add /path/to/video.mp4"
    echo "Start at login:    livewall autostart on"
else
    echo "It did not come up. Run '$PREFIX/bin/livewall probe' to see what is missing," >&2
    echo "or 'LIVEWALL_VERBOSE=1 $PREFIX/bin/livewall' to watch it start." >&2
    exit 1
fi
