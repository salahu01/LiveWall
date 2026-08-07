#!/usr/bin/env bash
#
# Sample the wallpaper's memory and CPU on the attached device.
#
#   ./tools/measure.sh [seconds]
#
# The macOS port quotes `footprint(1)` for memory and `top`'s one-second
# interval for CPU. The equivalents here are `dumpsys meminfo` (PSS, the figure
# that divides shared pages by the number of processes sharing them) and
# /proc/<pid>/stat sampled over the window, which is a percentage of one core —
# the same denominator the macOS numbers use.
#
# Run it twice to get the number that matters: once with the launcher visible,
# once with any app in front. The second should read zero, because the decoder
# has been released rather than paused.

set -euo pipefail

PACKAGE="com.fegno.livewall"
SECONDS_TO_SAMPLE="${1:-10}"

PID="$(adb shell pidof "$PACKAGE" | tr -d '\r' | awk '{print $1}')"
if [[ -z "$PID" ]]; then
    echo "$PACKAGE is not running. Set it as the wallpaper first." >&2
    exit 1
fi

CLOCK_TICKS="$(adb shell getconf CLK_TCK | tr -d '\r')"

read_jiffies() {
    adb shell cat "/proc/$PID/stat" | tr -d '\r' | awk '{print $14 + $15}'
}

before="$(read_jiffies)"
sleep "$SECONDS_TO_SAMPLE"
after="$(read_jiffies)"

cpu="$(awk -v a="$before" -v b="$after" -v t="$SECONDS_TO_SAMPLE" -v hz="$CLOCK_TICKS" \
    'BEGIN { printf "%.2f", (b - a) / hz / t * 100 }')"

pss="$(adb shell dumpsys meminfo "$PID" \
    | tr -d '\r' \
    | awk '/TOTAL PSS:/ { print $3; exit }')"

echo "pid $PID over ${SECONDS_TO_SAMPLE}s"
echo "  memory  $(awk -v k="$pss" 'BEGIN { printf "%.1f MB", k / 1024 }')"
echo "  cpu     ${cpu}% of one core"
