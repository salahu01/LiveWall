package com.fegno.livewall.importer

import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** Integer pixel dimensions. Deliberately not `android.util.Size`, so the
 *  sizing rules below stay testable on a plain JVM. */
data class Dimensions(val width: Int, val height: Int) {
    override fun toString() = "${width}×${height}"
}

/**
 * What the output should be sized and paced against, sourced from the panel the
 * wallpaper will actually play on.
 */
data class DisplayTarget(
    val pixelWidth: Int,
    val pixelHeight: Int,
    val maximumFramesPerSecond: Int
) {
    companion object {
        val FALLBACK = DisplayTarget(1080, 2400, 60)
    }
}

/**
 * The output geometry rules, kept apart from [Transcoder] so the test suite can
 * exercise them without an encoder, a display or an `android.jar`.
 */
object Sizing {

    /**
     * Largest frame rate no greater than [preferred] that divides [refresh]
     * exactly. Falls back to [preferred] when nothing sensible divides it.
     *
     * A frame arriving between two refreshes is decoded and then dropped by the
     * compositor, which is the most wasteful thing this pipeline can do. 24
     * divides 120 exactly; on a 60 Hz panel it does not, and 20 is both smoother
     * and cheaper.
     *
     * Phones make this matter more than laptops did: 90 Hz and 120 Hz panels are
     * ordinary, and 24 divides neither 90 nor 144.
     */
    fun pacedFps(preferred: Int, refresh: Int): Int {
        if (preferred <= 0 || refresh <= 0) return max(1, preferred)
        var candidate = min(preferred, refresh)
        // 12 fps is the floor worth snapping to; below that the cure is worse
        // than the judder.
        while (candidate >= 12) {
            if (refresh % candidate == 0) return candidate
            candidate--
        }
        return preferred
    }

    /**
     * Output dimensions for a preset.
     *
     * A fixed `maxEdge` fits the source inside that edge. `null` sizes to the
     * display instead — scaled so the frame *covers* the panel, since anything
     * less is upscaled at playback and that upscale was the single largest
     * quality loss in the pipeline. Neither path ever scales above 1:1: the
     * source has no detail past its own resolution and inventing pixels only
     * costs memory.
     *
     * [source] is expected to be orientation-corrected already — a 1080×1920
     * clip stored as 1920×1080 with a 90° rotation flag arrives here as
     * 1080×1920.
     */
    fun outputSize(source: Dimensions, preset: Preset, display: DisplayTarget): Dimensions {
        if (source.width <= 0 || source.height <= 0) {
            val edge = preset.maxEdge ?: display.pixelWidth
            return even(edge.toDouble(), edge * 9.0 / 16.0)
        }

        val scale: Double = if (preset.maxEdge != null) {
            val longest = max(source.width, source.height).toDouble()
            if (longest > preset.maxEdge) preset.maxEdge / longest else 1.0
        } else {
            val cover = max(
                display.pixelWidth.toDouble() / source.width,
                display.pixelHeight.toDouble() / source.height
            )
            min(cover, 1.0)
        }

        return even(source.width * scale, source.height * scale)
    }

    /** HEVC 4:2:0 requires even dimensions. */
    private fun even(width: Double, height: Double): Dimensions {
        val w = width.roundToInt()
        val h = height.roundToInt()
        return Dimensions(max(2, w - w % 2), max(2, h - h % 2))
    }
}
