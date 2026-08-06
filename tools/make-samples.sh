#!/bin/bash
# Regenerates the sample wallpapers in Samples/ and their poster frames.
#
#   ./tools/make-samples.sh
#
# The field is the one from Resources/wallpaper.metal — sinusoids crossed into a
# drifting plasma — evaluated by ffmpeg's `geq` instead of on the GPU.
#
# Every time term is a whole number of cycles over DURATION, which is what makes
# the loop seamless: the frame after the last is the first. Change DURATION and
# the loop stays seamless; change a spatial frequency and it also stays
# seamless. Change `2*PI*T/10` to something that isn't a whole multiple of the
# duration and it will not.
#
# Generated rather than sourced so the samples are original work under this
# repository's licence, with nothing to attribute.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)}"
DURATION=10
SIZE=1920x1080
FPS=24

if [ ! -x "$FFMPEG" ]; then
    echo "ffmpeg not found. Set FFMPEG=/path/to/ffmpeg" >&2
    exit 1
fi

mkdir -p "$ROOT/Samples" "$ROOT/docs"

# name, then three RGB triples: base colour, sweep colour, highlight colour.
generate() {
    local name=$1 r0=$2 g0=$3 b0=$4 r1=$5 g1=$6 b1=$7 r2=$8 g2=$9 b2=${10}
    local ux="(X/W)" uy="(1-Y/H)"

    local field="((0.5+0.5*sin(6*$ux+2*PI*T/$DURATION))\
+(0.5+0.5*cos(7*$uy-2*PI*T/$DURATION))\
+(0.5+0.5*sin(5*(0.7*$ux+0.7*$uy)+4*PI*T/$DURATION)))/3"
    local glow="pow($field,3)"
    # Slight corner falloff so the frame has a centre.
    local vignette="(1-0.35*(pow($ux-0.5,2)+pow($uy-0.5,2)))"

    echo "==> $name"
    "$FFMPEG" -y -v error -f lavfi -i "color=c=black:s=$SIZE:r=$FPS:d=$DURATION" \
        -vf "geq=\
r='255*$vignette*(($r0+($r1-$r0)*$field)+$r2*$glow)':\
g='255*$vignette*(($g0+($g1-$g0)*$field)+$g2*$glow)':\
b='255*$vignette*(($b0+($b1-$b0)*$field)+$b2*$glow)'" \
        -c:v libx264 -profile:v high -pix_fmt yuv420p -crf 20 -movflags +faststart \
        "$ROOT/Samples/$name.mp4"

    "$FFMPEG" -y -v error -ss 3 -i "$ROOT/Samples/$name.mp4" \
        -frames:v 1 -vf scale=800:-1 -q:v 4 "$ROOT/docs/sample-$name.jpg"

    echo "    $(du -h "$ROOT/Samples/$name.mp4" | cut -f1)"
}

generate aurora  0.02 0.04 0.12   0.08 0.42 0.52   0.35 0.18 0.55
generate ember   0.05 0.02 0.04   0.52 0.14 0.06   0.45 0.30 0.05

echo
echo "Check a seam with:"
echo "  ffmpeg -sseof -0.05 -i Samples/aurora.mp4 -frames:v 1 last.png"
echo "  ffmpeg -i Samples/aurora.mp4 -vf 'select=eq(n\\,0)' -frames:v 1 first.png"
echo "  ffmpeg -i last.png -i first.png -lavfi ssim -f null -   # expect ~0.99"
