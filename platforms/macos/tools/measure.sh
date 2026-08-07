#!/bin/bash
# Samples LiveWall's real cost so the low-resource claim is checkable rather
# than asserted.
#
#   ./tools/measure.sh            # 20 samples, 1s apart
#   ./tools/measure.sh 60         # 60 samples
#
# Run it once with the desktop visible, then again with a full-screen window
# covering the desktop. The second run is the number that matters — it should
# read 0.0% CPU and a footprint back near the idle baseline.
set -uo pipefail

SAMPLES="${1:-20}"
PID="$(pgrep -x LiveWall | head -1)"

if [ -z "$PID" ]; then
    echo "LiveWall is not running." >&2
    exit 1
fi

echo "pid $PID — $SAMPLES samples, 1s apart"
printf "%8s  %10s  %8s\n" "sample" "footprint" "cpu%"

total_cpu=0
peak_mem=0

for i in $(seq 1 "$SAMPLES"); do
    # `footprint` prints e.g. "LiveWall [123]: 64-bit  Footprint: 34 MB (...)".
    # This is the same number Activity Monitor calls "Memory"; RSS is not, and
    # notably misses GPU-owned pages, so there is no silent fallback to it.
    mem="$(footprint -p "$PID" 2>/dev/null | awk '/Footprint:/ {
        for (i = 1; i <= NF; i++) if ($i == "Footprint:") { print $(i+1) " " $(i+2); exit }
    }')"
    [ -z "$mem" ] && mem="unavailable"
    # `ps -o %cpu` reports an average over the process's whole lifetime, which
    # hides exactly what this script exists to show. top's second sample is a
    # real one-second interval.
    cpu="$(top -l 2 -s 1 -pid "$PID" -stats cpu 2>/dev/null | tail -1 | tr -d ' ')"
    [ -z "$cpu" ] && cpu="0.0"

    printf "%8s  %10s  %8s\n" "$i" "$mem" "$cpu"
    total_cpu="$(echo "$total_cpu $cpu" | awk '{print $1 + $2}')"
done

echo
echo "mean cpu: $(echo "$total_cpu $SAMPLES" | awk '{printf "%.2f%%", $1/$2}')"
echo
echo "For GPU and media-engine draw (needs sudo):"
echo "  sudo powermetrics --samplers gpu_power -i 1000 -n 5"
