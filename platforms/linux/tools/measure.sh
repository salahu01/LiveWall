#!/usr/bin/env bash
#
# Samples the running daemon's memory and CPU, so the README's numbers can be
# reproduced rather than taken on trust.
#
#   ./tools/measure.sh [seconds]
#
# Memory is Pss from /proc/<pid>/smaps_rollup — the same figure `livewall
# status` reports, and the closest Linux equivalent to the phys_footprint the
# macOS README quotes. Not RSS: an EGL client maps a large amount of the
# driver's shared state and RSS charges this process for all of it.
#
# CPU is a delta of utime+stime over the sampling window, expressed as a
# percentage of one core — the same unit `top` uses.
#
# To measure the *playback* cost on a machine whose desktop is behind a
# terminal, start the daemon with LIVEWALL_FORCE_RENDER=1. Without it the
# occlusion gate tears the decoder down and every reading comes back as the
# idle baseline, which is the correct behaviour and a useless measurement.
set -euo pipefail

DURATION="${1:-20}"

pid="$(pgrep -x livewall | head -1 || true)"
if [ -z "$pid" ]; then
    echo "LiveWall is not running." >&2
    exit 1
fi

clock_ticks="$(getconf CLK_TCK)"

read_cpu_ticks() {
    # Fields 14 and 15 of /proc/<pid>/stat are utime and stime. The process
    # name in field 2 can contain spaces and parentheses, so everything up to
    # the last ')' is discarded before counting.
    local stat
    stat="$(cut -d')' -f2- "/proc/$pid/stat")"
    # shellcheck disable=SC2086
    set -- $stat
    echo $(( $12 + $13 ))
}

read_pss_kb() {
    awk '/^Pss:/ { print $2; exit }' "/proc/$pid/smaps_rollup" 2>/dev/null || echo 0
}

echo "Sampling pid $pid for ${DURATION}s…"
echo

before_cpu="$(read_cpu_ticks)"
pss_total=0
pss_peak=0
samples=0

for _ in $(seq "$DURATION"); do
    sleep 1
    pss="$(read_pss_kb)"
    [ "$pss" -eq 0 ] && continue
    pss_total=$(( pss_total + pss ))
    [ "$pss" -gt "$pss_peak" ] && pss_peak="$pss"
    samples=$(( samples + 1 ))
done

after_cpu="$(read_cpu_ticks)"

if [ "$samples" -eq 0 ]; then
    echo "The process went away while sampling." >&2
    exit 1
fi

cpu_percent="$(awk -v d="$(( after_cpu - before_cpu ))" -v t="$clock_ticks" -v s="$DURATION" \
    'BEGIN { printf "%.2f", (d / t) / s * 100 }')"
mem_avg="$(awk -v k="$pss_total" -v n="$samples" 'BEGIN { printf "%.1f", k / n / 1024 }')"
mem_peak="$(awk -v k="$pss_peak" 'BEGIN { printf "%.1f", k / 1024 }')"

printf "memory (Pss)  %s MB average, %s MB peak\n" "$mem_avg" "$mem_peak"
printf "cpu           %s%% of one core\n" "$cpu_percent"
echo
livewall status 2>/dev/null | sed -n '1,3p' || true
