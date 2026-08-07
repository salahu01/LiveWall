package com.fegno.livewall.importer

/**
 * The two choices that belong to one video rather than to the library.
 *
 * [Preset] carries the settings a user picks once and forgets — quality, size,
 * bit depth. These two are different in kind: a clip shot sideways needs a
 * quarter turn that no other clip needs, and a slideshow of stills is fine at
 * 8 fps where a drifting nebula is not. Asking once per import is the only place
 * either question can be answered, so they live here and not in `Settings`.
 *
 * Both fields are deliberately requests rather than results. The frame rate is
 * still capped to the source and still snapped to the panel by
 * [Sizing.pacedFps]; the rotation is still composed with whatever orientation
 * flag the source already carries. What the user picks is the input to those
 * rules, not an escape from them.
 */
data class ImportOptions(
    /**
     * Frames per second to aim for, or `null` to take the preset's rate.
     *
     * Aim for, not land on: a 60 fps request against a 30 fps source is capped
     * to 30 — the frames simply are not there — and 24 against a 60 Hz panel
     * still snaps to 20, because a frame arriving between two refreshes is
     * decoded and then thrown away by the compositor.
     */
    val fps: Int? = null,

    /**
     * A quarter turn applied on top of the source's own rotation flag.
     *
     * Additive rather than absolute: a portrait clip already stored with a 90°
     * flag and rotated another 90° here ends up at 180°, which is what someone
     * pressing "rotate" twice expects. Setting it absolutely would silently
     * undo the source's own orientation for the first press.
     */
    val rotationDegrees: Int = 0
) {

    /** The rate to aim for before the source cap and the panel snap. */
    fun preferredFps(preset: Preset): Int = fps?.let(::sanitisedFps) ?: preset.fps

    /**
     * The source's orientation flag and the user's quarter turn as one angle.
     *
     * Both are normalised into 0…359 first: `MediaFormat.KEY_ROTATION` is
     * documented as a multiple of 90 but is read off a file the app did not
     * write, and a negative or out-of-range value there must not turn into a
     * negative modulo below.
     */
    fun effectiveRotation(sourceRotation: Int): Int =
        normalised(normalised(sourceRotation) + normalised(rotationDegrees))

    /** Label for the import dialog's rotation row. */
    val rotationLabel: String
        get() = when (normalised(rotationDegrees)) {
            0 -> "None"
            else -> "${normalised(rotationDegrees)}°"
        }

    companion object {
        val DEFAULT = ImportOptions()

        /**
         * The floor is [Sizing]'s own snapping floor: below 12 fps the pacer
         * stops looking for a divisor and the request passes through unsnapped,
         * so offering less would hand back judder the rest of the pipeline works
         * to avoid. The ceiling is well past anything a wallpaper wants and
         * exists only to keep a typo out of the encoder's bitrate maths.
         */
        const val MIN_FPS = 12
        const val MAX_FPS = 120

        /** The quarter turns offered, in the order a "rotate" button walks. */
        val ROTATIONS = listOf(0, 90, 180, 270)

        /** Clamps a typed-in rate into [MIN_FPS]…[MAX_FPS]. */
        fun sanitisedFps(value: Int): Int = value.coerceIn(MIN_FPS, MAX_FPS)

        /** Any integer into 0…359. Kotlin's `%` keeps the sign; this does not. */
        fun normalised(degrees: Int): Int = ((degrees % 360) + 360) % 360
    }
}
