package com.fegno.livewall.render

/**
 * A renderer that draws into the wallpaper's surface.
 *
 * The contract that makes the whole app cheap:
 *
 * - [activate] builds whatever decode resources are needed and starts drawing.
 * - [deactivate] **releases those resources** rather than merely pausing a
 *   callback, and leaves the last drawn frame posted, so re-activating never
 *   flashes black. A deactivated source must cost zero CPU and zero GPU.
 *
 * All methods are called on the engine's render thread.
 */
interface WallpaperSource {

    /** Short label for the settings screen. */
    val summary: String

    val isActive: Boolean

    fun onSurfaceSizeChanged(width: Int, height: Int) {}

    /**
     * How the frame maps onto a surface of a different aspect ratio. Procedural
     * sources draw to whatever bounds they are handed, so they have no aspect
     * ratio of their own and nothing to fit.
     */
    fun setFitMode(mode: FitMode) {}

    fun activate()

    fun deactivate()

    /** Called when the source is being discarded outright, not merely stopped. */
    fun release()
}
