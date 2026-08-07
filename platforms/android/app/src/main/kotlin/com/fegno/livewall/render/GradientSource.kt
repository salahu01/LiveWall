package com.fegno.livewall.render

import android.view.Choreographer
import com.fegno.livewall.support.Footprint
import com.fegno.livewall.support.Log
import kotlin.math.abs

/**
 * The procedural mode, and the fallback whenever no video is selected.
 *
 * **This is the one place the macOS design does not transfer, and it is worth
 * being blunt about it.** There, the equivalent hands `CAGradientLayer` colour
 * and location keyframes to the render server once; WindowServer interpolates
 * them on the GPU from then on, and the app's own cost is *literally* zero — no
 * timer, no callback, no per-frame work of any kind. Android has no such
 * facility. SurfaceFlinger composites the buffer you posted and will not animate
 * it for you, so a drifting gradient has to be redrawn from this process.
 *
 * What it costs, then: a `glClear`, four vertices, a three-stop mix per pixel,
 * and an `eglSwapBuffers`, ten times a second. The fragment shader is trivial
 * and the GPU is idle anyway, but the wake-up is real and this mode is not free
 * the way its macOS counterpart is.
 *
 * Ten frames a second rather than sixty because the drift takes half a minute to
 * cross the palette: at that speed the difference between consecutive frames is
 * far below a display's ability to show it, and the frames above ten are spent
 * redrawing an image nobody can tell apart from the last one.
 */
class GradientSource(private val target: RenderTarget) : WallpaperSource {

    override val summary: String get() = "Gradient"

    override var isActive = false
        private set

    private var choreographer: Choreographer? = null
    private var lastDrawNs = 0L
    private var startNs = 0L

    private val color0 = FloatArray(3)
    private val color1 = FloatArray(3)
    private val color2 = FloatArray(3)

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!isActive) return
            choreographer?.postFrameCallback(this)
            if (frameTimeNanos - lastDrawNs < FRAME_INTERVAL_NS) return
            lastDrawNs = frameTimeNanos
            if (startNs == 0L) startNs = frameTimeNanos
            draw((frameTimeNanos - startNs) / 1_000_000_000.0)
        }
    }

    override fun activate() {
        if (isActive) return
        isActive = true
        lastDrawNs = 0L
        choreographer = Choreographer.getInstance()
        choreographer?.postFrameCallback(frameCallback)
        Log.info("gradient activated — ${Footprint.formatted()}")
    }

    override fun deactivate() {
        if (!isActive) return
        isActive = false
        choreographer?.removeFrameCallback(frameCallback)
        choreographer = null
        // No clear and no swap: the last drawn frame stays posted, so stopping
        // freezes the drift rather than blanking the screen. `startNs` is kept
        // so resuming continues the cycle instead of snapping back to the top.
        Log.info("gradient deactivated — ${Footprint.formatted()}")
    }

    override fun release() {
        deactivate()
    }

    private fun draw(elapsedSeconds: Double) {
        // Two triangle waves on deliberately coprime-ish periods, so the pair
        // does not visibly repeat on any timescale a person watches a wallpaper
        // over.
        val colourPhase = triangle(elapsedSeconds / COLOUR_PERIOD)
        val locationPhase = triangle(elapsedSeconds / LOCATION_PERIOD)

        mix(PALETTE_A[0], PALETTE_B[0], colourPhase, color0)
        mix(PALETTE_A[1], PALETTE_B[1], colourPhase, color1)
        mix(PALETTE_A[2], PALETTE_B[2], colourPhase, color2)
        val mid = (0.35 + 0.35 * locationPhase).toFloat()

        target.makeCurrent()
        target.clearToBlack()
        target.gradientProgram().draw(color0, color1, color2, mid)
        target.swap()
    }

    /** 0 → 1 → 0 over one unit of [t], with no discontinuity at the turn. */
    private fun triangle(t: Double): Double {
        val phase = t - Math.floor(t)
        val linear = abs(phase * 2 - 1)
        // Smoothstep, so the reversal is not a visible kink in the drift.
        val eased = linear * linear * (3 - 2 * linear)
        return 1 - eased
    }

    private fun mix(from: FloatArray, to: FloatArray, amount: Double, into: FloatArray) {
        val a = amount.toFloat()
        for (channel in 0..2) into[channel] = from[channel] + (to[channel] - from[channel]) * a
    }

    private companion object {
        const val FRAME_INTERVAL_NS = 1_000_000_000L / 10
        const val COLOUR_PERIOD = 24.0
        const val LOCATION_PERIOD = 37.0

        // The macOS port's palette, in linear-ish sRGB floats.
        val PALETTE_A = arrayOf(
            floatArrayOf(0.04f, 0.05f, 0.11f),
            floatArrayOf(0.16f, 0.10f, 0.30f),
            floatArrayOf(0.03f, 0.19f, 0.24f)
        )
        val PALETTE_B = arrayOf(
            floatArrayOf(0.06f, 0.04f, 0.16f),
            floatArrayOf(0.09f, 0.16f, 0.32f),
            floatArrayOf(0.02f, 0.12f, 0.18f)
        )
    }
}
