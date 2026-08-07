package com.fegno.livewall.support

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import com.fegno.livewall.importer.DisplayTarget
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The panel the wallpaper will actually play on.
 *
 * `Mode.physicalWidth`/`physicalHeight` rather than anything from
 * `DisplayMetrics`: the app needs real pixels, and a `Configuration`-derived
 * size is in the current orientation, excludes system bars on some OEM builds,
 * and changes when the user rotates the phone. The panel does not.
 *
 * The refresh rate is the *highest* the panel supports, not the one it happens
 * to be running at. A 120 Hz phone drops to 60 to save power and comes back the
 * moment something animates, and a file paced to divide 60 judders on the way
 * back up. Pacing to the maximum divides both.
 */
fun currentDisplayTarget(context: Context): DisplayTarget {
    val manager = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
        ?: return DisplayTarget.FALLBACK
    val display = manager.getDisplay(Display.DEFAULT_DISPLAY) ?: return DisplayTarget.FALLBACK

    val mode = display.mode ?: return DisplayTarget.FALLBACK
    val refresh = display.supportedModes
        ?.maxOfOrNull { it.refreshRate }
        ?: mode.refreshRate

    val width = mode.physicalWidth
    val height = mode.physicalHeight
    if (width <= 0 || height <= 0) return DisplayTarget.FALLBACK

    return DisplayTarget(
        pixelWidth = width,
        pixelHeight = height,
        maximumFramesPerSecond = max(1, refresh.roundToInt())
    )
}
