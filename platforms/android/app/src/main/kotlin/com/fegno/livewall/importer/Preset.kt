package com.fegno.livewall.importer

/**
 * Import presets.
 *
 * The macOS port measured, against its real playback path, that going from
 * 1280×720 to 3492×1964 (7.4× the pixels) moved the footprint 15 MB → 18 MB and
 * left CPU inside the noise, while going from 24 to 60 fps took CPU from 3.5% to
 * 7.8%. HEVC decode is fixed-function on the media engine and flat in frame
 * size; what remains is the app's own per-frame pump work, which scales with
 * frames per second and nothing else.
 *
 * Mobile SoCs have the same shape of media block, and the conclusion carries:
 * spend freely on resolution, bit depth and bitrate — all close to free at
 * playback — and stay frugal with frame rate, the one knob that costs.
 *
 * What does *not* carry is the indifference to it. A laptop paying 3% of a core
 * pays it from the wall; a phone pays it from a 5000 mAh battery it also has to
 * run the day on. So the default here is one step cheaper than the macOS
 * default: Balanced rather than Native.
 */
data class Preset(
    val name: String,
    /** Longest output edge in pixels, or `null` to size to the display. */
    val maxEdge: Int?,
    val fps: Int,
    /**
     * Bits per pixel per second. Costs storage and nothing else — on macOS the
     * 6.8 Mbps variant measured marginally *cheaper* to play than the 1.4 Mbps
     * one. Starving the smooth gradient content that makes good wallpaper is
     * what banding in smoke and glow looks like.
     */
    val bitsPerPixel: Double,
    /** 10-bit costs nothing to decode and is the real fix for banding. */
    val bitDepth: Int,
    /**
     * Keyframes are expensive at these bitrates and a wallpaper seeks only when
     * it resumes from being hidden, so they can be sparse.
     */
    val keyframeSeconds: Double
) {
    /** Label for the picker. `maxEdge == null` has no fixed number to show. */
    val summary: String
        get() = "${maxEdge?.let { "${it}p" } ?: "your screen"} · $fps fps · $bitDepth-bit"

    companion object {
        val ULTRA_LIGHT = Preset("Ultra Light", maxEdge = 960, fps = 20, bitsPerPixel = 0.10, bitDepth = 8, keyframeSeconds = 5.0)

        val BALANCED = Preset("Balanced", maxEdge = 1920, fps = 24, bitsPerPixel = 0.15, bitDepth = 8, keyframeSeconds = 5.0)

        /**
         * 24 rather than 30: frame rate is the one knob that costs CPU linearly,
         * it divides a 120 Hz panel as evenly as 30 does, and ambient loops gain
         * nothing from the extra six frames.
         */
        val NATIVE = Preset("Native", maxEdge = null, fps = 24, bitsPerPixel = 0.12, bitDepth = 10, keyframeSeconds = 5.0)

        val ALL = listOf(ULTRA_LIGHT, BALANCED, NATIVE)

        val DEFAULT = BALANCED

        fun named(name: String?): Preset = ALL.firstOrNull { it.name == name } ?: DEFAULT
    }
}
