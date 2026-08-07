package com.fegno.livewall.importer

import kotlin.math.max

/**
 * Resamples a stream of video frames onto an exact 1/fps grid.
 *
 * Frames arriving between grid points are dropped; the ones that survive are
 * stamped with clean, evenly spaced presentation times starting at zero. That
 * gives the encoder a genuinely constant frame rate rather than the source
 * cadence with a frame-rate hint attached.
 *
 * This matters beyond tidiness: the playback path pulls exactly one frame per
 * tick at the rate recorded in the library, so a file whose real rate differs
 * from its recorded rate plays at the wrong speed.
 *
 * Grid positions are computed from an index rather than accumulated, because
 * `1_000_000 / fps` is not an integer for most rates and adding a truncated
 * value a few thousand times walks the timeline off by whole frames.
 *
 * Not thread-safe: driven from the transcoder's single pump.
 */
class FramePacer(fps: Int) {

    private val fps = max(1, fps)

    var kept = 0
        private set
    var dropped = 0
        private set

    /** Next position on the source grid we still want a frame for. */
    private var nextSourceIndex = 0

    /**
     * Returns the presentation time to stamp on [presentationTimeUs], or `null`
     * if the frame lands between grid points and should be dropped.
     */
    fun accept(presentationTimeUs: Long): Long? {
        if (presentationTimeUs < gridUs(nextSourceIndex)) {
            dropped++
            return null
        }

        val output = gridUs(kept)
        kept++

        // Advance the source cursor past this frame so a source slower than the
        // target grid can't make us emit the same instant twice.
        do {
            nextSourceIndex++
        } while (gridUs(nextSourceIndex) <= presentationTimeUs)

        return output
    }

    private fun gridUs(index: Int): Long = index.toLong() * 1_000_000L / fps
}
