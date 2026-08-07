#!/bin/bash
# Builds LiveWall and assembles a runnable .app bundle.
#
# A bundle (rather than the bare SwiftPM executable) is required for a stable
# bundle identifier, LSUIElement, and the AppKit panels that depend on both.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:-release}"
APP="$ROOT/build/LiveWall.app"

# Both architectures, always.
#
# macOS 14 is the deployment target and Sonoma still runs on the 2018-2020 Intel
# machines, so a build for the host architecture alone silently excludes Macs
# that meet the stated requirement — and does it in a way nothing catches: the
# app assembles, signs and verifies, and then refuses to launch on the one
# machine that cannot be tested from an Apple silicon host.
#
# Set LIVEWALL_ARCHS to override (`LIVEWALL_ARCHS=arm64` halves the link time
# during ordinary development).
ARCHS="${LIVEWALL_ARCHS:-arm64 x86_64}"
ARCH_FLAGS=()
for arch in $ARCHS; do
    ARCH_FLAGS+=(--arch "$arch")
done

echo "==> Building ($CONFIG, ${ARCHS// /+})"
cd "$ROOT"
swift build -c "$CONFIG" "${ARCH_FLAGS[@]}"

# A multi-arch build lands in a per-target directory rather than
# .build/<config>/, so ask SwiftPM where it put things instead of guessing.
BIN="$(swift build -c "$CONFIG" "${ARCH_FLAGS[@]}" --show-bin-path)/LiveWall"
[ -f "$BIN" ] || { echo "build produced no binary at $BIN" >&2; exit 1; }

echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/LiveWall"
cp "$ROOT/Resources/Info.plist" "$APP/Contents/Info.plist"

if [ -f "$ROOT/Resources/AppIcon.icns" ]; then
    cp "$ROOT/Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
else
    echo "    No AppIcon.icns; regenerate with: swift tools/make-icon.swift"
fi

# Precompile the shader. Loading a metallib avoids pulling the Metal runtime
# compiler into the process, which measured at ~97 MB of resident graphics
# memory that is never released — by far the largest single cost in the app.
echo "==> Compiling shader (optional)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if xcrun -sdk macosx metal -O -c "$ROOT/Resources/wallpaper.metal" -o "$TMP/wallpaper.air" 2>"$TMP/metal.log" \
   && xcrun -sdk macosx metallib "$TMP/wallpaper.air" -o "$APP/Contents/Resources/wallpaper.metallib" 2>>"$TMP/metal.log"; then
    echo "    wallpaper.metallib built — Metal procedural mode available"
else
    echo "    Metal toolchain unavailable; skipping metallib."
    echo "    The app falls back to the Core Animation gradient, which is cheaper anyway."
    echo "    To enable the Metal shader mode: xcodebuild -downloadComponent MetalToolchain"
fi

# States what actually shipped. A missing slice is otherwise invisible until
# someone opens the app on the architecture that is absent.
echo "==> Architectures: $(lipo -archs "$APP/Contents/MacOS/LiveWall")"

# Distribution needs a Developer ID identity, the hardened runtime and a
# timestamp; Gatekeeper rejects an ad-hoc signature on any machine but the one
# that built it. Set SIGN_IDENTITY to switch modes:
#
#   SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" ./tools/bundle.sh
#
# Launch-at-login also wants a stable signature: SMAppService registrations are
# keyed to the bundle identity, so an ad-hoc build re-registers on every rebuild.
if [ -n "${SIGN_IDENTITY:-}" ]; then
    echo "==> Signing (Developer ID, hardened runtime)"
    codesign --force --deep --options runtime --timestamp \
             --sign "$SIGN_IDENTITY" "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"

    # Notarisation needs credentials stored once with:
    #   xcrun notarytool store-credentials livewall --apple-id … --team-id … --password …
    if [ -n "${NOTARY_PROFILE:-}" ]; then
        ZIP="$ROOT/build/LiveWall.zip"
        echo "==> Notarising (profile: $NOTARY_PROFILE)"
        ditto -c -k --keepParent "$APP" "$ZIP"
        xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
        xcrun stapler staple "$APP"
        rm -f "$ZIP"
        echo "    Stapled. Verify with: spctl -a -vvv '$APP'"
    else
        echo "    Set NOTARY_PROFILE to notarise and staple."
    fi
else
    echo "==> Signing (ad-hoc — this machine only)"
    codesign --force --deep --sign - "$APP"
    echo "    Set SIGN_IDENTITY for a distributable build."
fi

echo "==> Done: $APP"
echo "    Run with: open '$APP'"
echo "    Verbose:  LIVEWALL_VERBOSE=1 '$APP/Contents/MacOS/LiveWall'"
