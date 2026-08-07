#!/usr/bin/env bash
#
# Build, test and (optionally) install the Android port.
#
#   ./tools/build.sh            # debug APK + unit tests
#   ./tools/build.sh release    # release APK, minified and shrunk
#   ./tools/build.sh install    # debug APK onto the attached device, then open
#                               # the live-wallpaper preview
#
# The only prerequisites are a JDK 17 and an Android SDK. `sdk.dir` is read from
# local.properties if present, otherwise from ANDROID_HOME/ANDROID_SDK_ROOT.

set -euo pipefail

cd "$(dirname "$0")/.."

MODE="${1:-debug}"
PACKAGE="com.fegno.livewall"
SERVICE="$PACKAGE/.app.LiveWallService"

if [[ ! -f local.properties ]]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
    if [[ ! -d "$SDK" ]]; then
        echo "No Android SDK found. Set ANDROID_HOME or write local.properties." >&2
        exit 1
    fi
    echo "sdk.dir=$SDK" > local.properties
fi

case "$MODE" in
release)
    ./gradlew :app:testReleaseUnitTest :app:assembleRelease
    APK=app/build/outputs/apk/release/app-release-unsigned.apk
    echo
    echo "$APK — $(du -h "$APK" | cut -f1)"
    echo "Unsigned. To install it, sign with your own key:"
    echo "  apksigner sign --ks <keystore> --out LiveWall.apk $APK"
    ;;

install)
    ./gradlew :app:installDebug
    # Opens the system's live-wallpaper preview pointed straight at this
    # service, which is the only way to start a wallpaper without root.
    adb shell am start \
        -a android.service.wallpaper.CHANGE_LIVE_WALLPAPER \
        --es android.service.wallpaper.extra.LIVE_WALLPAPER_COMPONENT "$SERVICE" \
        --ecn android.service.wallpaper.extra.LIVE_WALLPAPER_COMPONENT "$SERVICE"
    echo
    echo "Verbose logging:"
    echo "  adb shell setprop log.tag.LiveWall VERBOSE && adb logcat -s LiveWall"
    ;;

debug | *)
    ./gradlew :app:testDebugUnitTest :app:assembleDebug
    APK=app/build/outputs/apk/debug/app-debug.apk
    echo
    echo "$APK — $(du -h "$APK" | cut -f1)"
    ;;
esac
