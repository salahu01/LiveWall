package com.fegno.livewall.render

import kotlin.math.abs

/**
 * How a wallpaper frame is mapped onto a display whose aspect ratio doesn't
 * match it.
 *
 * Purely a property of the draw call — a two-component scale applied to the
 * full-screen quad in clip space — so switching modes is free: no re-encode, no
 * reload, no decoder involvement, nothing on disk changes.
 *
 * One difference from the macOS port worth stating plainly. There, `.fit`'s
 * letterbox bars are transparent and show the system's own wallpaper picture
 * underneath. On Android this *is* the wallpaper; there is nothing beneath it,
 * so the bars are the clear colour and the clear colour is black.
 */
enum class FitMode(val key: String) {

    /** Scale until both axes are covered and let the overflow clip. Default. */
    FILL("fill"),

    /** Scale until the whole frame is visible and letterbox the remainder. */
    FIT("fit"),

    /** Scale each axis independently. Fills the display, distorts the picture. */
    STRETCH("stretch");

    val title: String
        get() = when (this) {
            FILL -> "Fill Screen"
            FIT -> "Fit to Screen"
            STRETCH -> "Stretch"
        }

    /** Names the cost, since every mode has one. */
    val tradeoff: String
        get() = when (this) {
            FILL -> "crops edges"
            FIT -> "adds bars"
            STRETCH -> "distorts"
        }

    /**
     * Clip-space scale for a full-screen quad whose vertices are the corners of
     * the unit square in NDC.
     *
     * `FILL` scales past 1 on one axis and lets the rasteriser clip; `FIT`
     * scales below 1 and leaves the clear colour showing. Both are one uniform
     * write per frame, which is why mode switching costs nothing.
     */
    fun scale(contentWidth: Int, contentHeight: Int, surfaceWidth: Int, surfaceHeight: Int): FloatArray {
        if (contentWidth <= 0 || contentHeight <= 0 || surfaceWidth <= 0 || surfaceHeight <= 0) {
            return floatArrayOf(1f, 1f)
        }
        val content = contentWidth.toFloat() / contentHeight
        val surface = surfaceWidth.toFloat() / surfaceHeight
        val ratio = content / surface
        return when (this) {
            STRETCH -> floatArrayOf(1f, 1f)
            FILL -> if (ratio > 1f) floatArrayOf(ratio, 1f) else floatArrayOf(1f, 1f / ratio)
            FIT -> if (ratio > 1f) floatArrayOf(1f, 1f / ratio) else floatArrayOf(ratio, 1f)
        }
    }

    /**
     * What this mode actually costs for [contentWidth]×[contentHeight] shown on
     * [surfaceWidth]×[surfaceHeight], as a sentence for the settings screen.
     * `null` when the aspect ratios agree closely enough that no mode does
     * anything visible.
     *
     * Worth surfacing: "why is my wallpaper cropped" is answered by two aspect
     * ratios the user can't see anywhere else in the UI — and on a phone, where
     * almost every source clip is the wrong shape for a 20:9 panel, the answer
     * is usually "by a lot".
     */
    fun effectDescription(
        contentWidth: Int,
        contentHeight: Int,
        surfaceWidth: Int,
        surfaceHeight: Int
    ): String? {
        if (contentWidth <= 0 || contentHeight <= 0 || surfaceWidth <= 0 || surfaceHeight <= 0) return null

        val content = contentWidth.toDouble() / contentHeight
        val surface = surfaceWidth.toDouble() / surfaceHeight
        val ratio = content / surface
        if (abs(ratio - 1.0) <= 0.005) return null

        fun percent(value: Double) = "${Math.round(value * 100)}%"

        return when (this) {
            FILL -> if (ratio > 1)
                "Crops ${percent(1 - 1 / ratio)} of the width."
            else
                "Crops ${percent(1 - ratio)} of the height."

            FIT -> if (ratio > 1)
                "Bars above and below, ${percent(1 - 1 / ratio)} of the height."
            else
                "Bars left and right, ${percent(1 - ratio)} of the width."

            STRETCH -> if (ratio > 1)
                "Squeezes the picture ${percent(1 - 1 / ratio)} horizontally."
            else
                "Stretches the picture ${percent(1 / ratio - 1)} horizontally."
        }
    }

    companion object {
        val DEFAULT = FILL

        fun fromKey(key: String?): FitMode = entries.firstOrNull { it.key == key } ?: DEFAULT
    }
}
