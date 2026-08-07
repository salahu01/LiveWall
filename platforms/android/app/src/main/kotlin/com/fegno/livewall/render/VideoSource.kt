package com.fegno.livewall.render

import android.graphics.SurfaceTexture
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.opengl.GLES20
import android.os.Handler
import android.os.SystemClock
import android.view.Choreographer
import android.view.Surface
import com.fegno.livewall.support.Footprint
import com.fegno.livewall.support.Log
import java.io.File
import kotlin.math.max

/**
 * Plays a looping video onto the wallpaper surface.
 *
 * Memory discipline, in order of how much each one saves:
 *
 * 1. **One frame in flight.** A `Choreographer` callback pulls exactly one
 *    decoded frame per tick and draws it. There is no read-ahead queue and no
 *    media clock, so the only decoded frames resident are the ones the codec's
 *    own output buffer set holds. An `ExoPlayer`/`MediaPlayer` on the same file
 *    would buffer ahead by an amount the app does not control, and at these
 *    resolutions one 10-bit frame is ~20 MB.
 * 2. **The decoder's own format, requested from nowhere.** The output goes to a
 *    `SurfaceTexture` and is sampled as an external texture, so hardware HEVC
 *    hands back 4:2:0 at the file's bit depth — 1.5 bytes/px at 8-bit against
 *    RGBA's 4 — and the YUV→RGB conversion is the sampler's fixed-function job.
 *    Routing through an `ImageReader` to name a format would add a full-frame
 *    copy per frame for nothing.
 * 3. **Teardown on hide.** Losing visibility releases the `MediaCodec` and its
 *    buffers outright. The last frame stays posted on the surface at no cost,
 *    and playback resumes from the saved timestamp.
 *
 * The `MediaExtractor` deliberately survives teardown. It holds a file
 * descriptor and a parsed sample table, not pixels, and keeping it is what makes
 * every activate after the first one synchronous and fast — the same reason the
 * macOS port caches its `AVAsset` and rebuilds only the reader.
 *
 * Assets are expected to have come out of `Transcoder` first: HEVC, no audio
 * track, no B-frames, capped resolution and frame rate.
 */
class VideoSource(
    private val file: File,
    indexFps: Int,
    private val bitDepth: Int,
    private var fitMode: FitMode,
    private val target: RenderTarget,
    private val frameHandler: Handler
) : WallpaperSource {

    override val summary: String get() = file.nameWithoutExtension

    override var isActive = false
        private set

    /** True once the track has been found and its geometry read. */
    private var prepared = false
    private var failed = false

    private var extractor: MediaExtractor? = null
    private var trackIndex = -1
    private var mime: String? = null

    private var videoWidth = 0
    private var videoHeight = 0
    private var rotationDegrees = 0
    private var durationUs = 0L

    /**
     * Tick rate. Seeded from the library's record but replaced by the track's
     * real rate once the format is read: pulling one frame per tick means a
     * mismatch between the two plays the clip at the wrong speed, and the file
     * is the authority, not the index.
     */
    private var targetFps = max(1, indexFps)
    private var frameIntervalNs = 1_000_000_000L / targetFps

    private var decoder: MediaCodec? = null
    private var decoderSurface: Surface? = null
    private var surfaceTexture: SurfaceTexture? = null
    private var textureId = 0

    /** Playback position preserved across teardown so resuming continues the
     *  loop instead of snapping back to frame zero. */
    private var resumeUs = 0L

    /** After a resume seek lands on the preceding sync frame, everything before
     *  this is decoded and thrown away rather than drawn. */
    private var skipUntilUs = 0L

    private var inputDone = false
    private var lastRenderNs = 0L
    private var framesDrawn = 0L

    private val bufferInfo = MediaCodec.BufferInfo()
    private val texMatrix = FloatArray(16)

    private val frameLock = java.lang.Object()
    private var frameAvailable = false

    private var choreographer: Choreographer? = null
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!isActive) return
            // Re-post first: an exception in the pump should not silently stop
            // the wallpaper for the rest of the session.
            choreographer?.postFrameCallback(this)

            // Android has no equivalent of pinning a display link to the asset's
            // frame rate — the callback arrives at the panel's refresh, whatever
            // that is — so the rate cap happens here. An empty tick is a
            // timestamp comparison; the transcoder snaps the file's frame rate to
            // a divisor of the refresh precisely so this gate lands evenly.
            if (frameTimeNanos - lastRenderNs < frameIntervalNs) return
            lastRenderNs = frameTimeNanos
            pump()
        }
    }

    // MARK: - Preparation

    /**
     * Opens the file and reads the video track's geometry. Cheap enough to do on
     * the render thread — it parses a header, it does not decode.
     */
    fun prepare(): Boolean {
        if (prepared) return true
        if (failed) return false

        val candidate = MediaExtractor()
        try {
            candidate.setDataSource(file.absolutePath)
            for (index in 0 until candidate.trackCount) {
                val format = candidate.getTrackFormat(index)
                val type = format.getString(MediaFormat.KEY_MIME) ?: continue
                if (!type.startsWith("video/")) continue

                candidate.selectTrack(index)
                trackIndex = index
                mime = type
                videoWidth = format.getInteger(MediaFormat.KEY_WIDTH)
                videoHeight = format.getInteger(MediaFormat.KEY_HEIGHT)
                rotationDegrees = format.getIntegerOrNull(MediaFormat.KEY_ROTATION) ?: 0
                durationUs = format.getLongOrNull(MediaFormat.KEY_DURATION) ?: 0L

                format.frameRateOrNull()?.let { rate ->
                    val actual = Math.round(rate)
                    if (actual >= 1 && actual != targetFps) {
                        Log.info("track is $actual fps, index said $targetFps — using the track")
                    }
                    if (actual >= 1) setTargetFps(actual)
                }

                // A rotated source would need the output geometry swapped, but
                // Transcoder bakes rotation into the pixels and writes 0. If one
                // turns up anyway the texture matrix still handles it.
                if (rotationDegrees % 180 != 0) {
                    val swap = videoWidth
                    videoWidth = videoHeight
                    videoHeight = swap
                }

                extractor = candidate
                prepared = true
                return true
            }
            Log.error("${file.name} has no video track")
        } catch (error: Exception) {
            Log.error("could not open ${file.name}: $error")
        }

        candidate.release()
        failed = true
        return false
    }

    private fun setTargetFps(fps: Int) {
        targetFps = max(1, fps)
        frameIntervalNs = 1_000_000_000L / targetFps
    }

    // MARK: - WallpaperSource

    override fun onSurfaceSizeChanged(width: Int, height: Int) {
        // Nothing to rebuild: the fit scale is recomputed from the target's size
        // on every draw.
    }

    override fun setFitMode(mode: FitMode) {
        fitMode = mode
    }

    override fun activate() {
        if (isActive || failed) return
        if (!prepare()) return

        val extractor = this.extractor ?: return
        val mime = this.mime ?: return

        try {
            target.makeCurrent()
            textureId = OesQuadProgram.createExternalTexture()
            val texture = SurfaceTexture(textureId)
            texture.setDefaultBufferSize(videoWidth, videoHeight)
            // The listener must land on a *different* thread from the one that
            // waits for it, or the wait deadlocks the delivery.
            texture.setOnFrameAvailableListener({
                synchronized(frameLock) {
                    frameAvailable = true
                    frameLock.notifyAll()
                }
            }, frameHandler)
            surfaceTexture = texture

            val surface = Surface(texture)
            decoderSurface = surface

            val format = extractor.getTrackFormat(trackIndex)
            val codec = MediaCodec.createDecoderByType(mime)
            // No output-format request and no crypto: decoding straight to a
            // surface is the path the hardware block is built for.
            codec.configure(format, surface, null, 0)
            codec.start()
            decoder = codec

            inputDone = false
            frameAvailable = false
            lastRenderNs = 0L
            seekTo(resumeUs)

            isActive = true
            choreographer = Choreographer.getInstance()
            choreographer?.postFrameCallback(frameCallback)

            Log.info("video activated at ${resumeUs / 1_000_000.0}s — ${Footprint.formatted()}")
        } catch (error: Exception) {
            Log.error("could not start the decoder for ${file.name}: $error")
            teardownDecoder()
            failed = true
        }
    }

    override fun deactivate() {
        if (!isActive) return
        isActive = false

        choreographer?.removeFrameCallback(frameCallback)
        choreographer = null

        // Release the codec and its buffers. This is the difference between
        // "paused" and "costs nothing".
        //
        // Deliberately no clear and no swap — the last drawn frame stays posted,
        // so re-activating is seamless and a stopped wallpaper reads as a still.
        teardownDecoder()

        Log.info("video deactivated — ${Footprint.formatted()}")
    }

    override fun release() {
        deactivate()
        extractor?.release()
        extractor = null
        prepared = false
    }

    private fun teardownDecoder() {
        decoder?.let { codec ->
            runCatching { codec.stop() }
            runCatching { codec.release() }
        }
        decoder = null

        decoderSurface?.release()
        decoderSurface = null

        surfaceTexture?.let { texture ->
            texture.setOnFrameAvailableListener(null)
            texture.release()
        }
        surfaceTexture = null

        if (textureId != 0) {
            target.makeCurrent()
            GLES20.glDeleteTextures(1, intArrayOf(textureId), 0)
            textureId = 0
        }

        synchronized(frameLock) { frameAvailable = false }
    }

    // MARK: - The pump

    /**
     * Advances by at most one *drawn* frame.
     *
     * The step budget bounds two different things at once. Feeding compressed
     * access units is self-limiting — `dequeueInputBuffer` starts refusing once
     * the codec's small input set is full — but catching up after a resume seek
     * is not, and neither is a codec that wants several polls before it produces
     * anything. Sixty-four steps is enough to swallow a five-second keyframe gap
     * in two or three ticks and small enough that a stuck codec costs one tick,
     * not the frame budget.
     */
    private fun pump() {
        val codec = decoder ?: return
        var steps = 0

        while (steps++ < MAX_STEPS_PER_TICK) {
            feedInput(codec)

            val index = try {
                codec.dequeueOutputBuffer(bufferInfo, DEQUEUE_TIMEOUT_US)
            } catch (error: IllegalStateException) {
                Log.error("decoder failed mid-playback: $error")
                restartLoop()
                return
            }

            when {
                index >= 0 -> {
                    val endOfStream = bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    val presentationTimeUs = bufferInfo.presentationTimeUs
                    val show = !endOfStream &&
                        bufferInfo.size > 0 &&
                        presentationTimeUs >= skipUntilUs

                    codec.releaseOutputBuffer(index, show)

                    if (endOfStream) {
                        restartLoop()
                        return
                    }
                    if (show) {
                        resumeUs = presentationTimeUs
                        drawLatestFrame()
                        return
                    }
                    // Otherwise: a frame between the sync point we seeked to and
                    // where we actually left off. Released without rendering,
                    // which costs the decode and nothing else.
                }

                index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    applyOutputFormat(codec.outputFormat)
                }

                index == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                    // The codec has nothing yet. Keep stepping so the input side
                    // gets another chance to feed it; the budget ends the tick.
                }
            }
        }
    }

    private fun feedInput(codec: MediaCodec) {
        if (inputDone) return
        val extractor = this.extractor ?: return

        val index = try {
            codec.dequeueInputBuffer(0)
        } catch (error: IllegalStateException) {
            Log.error("decoder rejected an input dequeue: $error")
            return
        }
        if (index < 0) return

        val buffer = codec.getInputBuffer(index) ?: return
        val size = extractor.readSampleData(buffer, 0)

        if (size < 0) {
            codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
            inputDone = true
            return
        }

        codec.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
        extractor.advance()
    }

    private fun drawLatestFrame() {
        val texture = surfaceTexture ?: return

        if (!awaitFrame()) {
            Log.error("timed out waiting for a decoded frame")
            return
        }

        target.makeCurrent()
        texture.updateTexImage()
        texture.getTransformMatrix(texMatrix)

        val matrix = OesQuadProgram.rotatedTexMatrix(texMatrix, rotationDegrees)
        val scale = fitMode.scale(videoWidth, videoHeight, target.width, target.height)

        // Cleared every frame because FILL and STRETCH cover the viewport but
        // FIT does not, and a stale band from the previous mode is worse than
        // the clear it costs to avoid.
        target.clearToBlack()
        target.oesProgram().draw(textureId, matrix, scale[0], scale[1])
        target.swap()

        framesDrawn++
        if (Log.verbose && framesDrawn % 240 == 0L) {
            Log.info(
                "frames=$framesDrawn pts=%.1fs %d-bit ${Footprint.formatted()}"
                    .format(resumeUs / 1_000_000.0, bitDepth)
            )
        }
    }

    /**
     * `releaseOutputBuffer(true)` hands the frame to the `SurfaceTexture`
     * asynchronously; `updateTexImage` before the buffer lands would re-bind the
     * previous one. The timeout is a safety valve, not an expected path — if it
     * fires, the tick is skipped and the next one tries again.
     */
    private fun awaitFrame(): Boolean {
        synchronized(frameLock) {
            val deadline = SystemClock.uptimeMillis() + FRAME_WAIT_MS
            while (!frameAvailable) {
                val remaining = deadline - SystemClock.uptimeMillis()
                if (remaining <= 0) return false
                try {
                    frameLock.wait(remaining)
                } catch (_: InterruptedException) {
                    return false
                }
            }
            frameAvailable = false
        }
        return true
    }

    private fun applyOutputFormat(format: MediaFormat) {
        // The output format carries the real crop rectangle. A decoder padding
        // 1080 up to 1088 and cropping back is ordinary, and using the padded
        // height as the aspect ratio would tilt every fit calculation.
        val left = format.getIntegerOrNull("crop-left")
        val right = format.getIntegerOrNull("crop-right")
        val top = format.getIntegerOrNull("crop-top")
        val bottom = format.getIntegerOrNull("crop-bottom")

        val width: Int
        val height: Int
        if (left != null && right != null && top != null && bottom != null) {
            width = right - left + 1
            height = bottom - top + 1
        } else {
            width = format.getIntegerOrNull(MediaFormat.KEY_WIDTH) ?: return
            height = format.getIntegerOrNull(MediaFormat.KEY_HEIGHT) ?: return
        }
        if (width <= 0 || height <= 0) return

        if (rotationDegrees % 180 == 0) {
            videoWidth = width
            videoHeight = height
        } else {
            videoWidth = height
            videoHeight = width
        }
    }

    private fun seekTo(positionUs: Long) {
        val extractor = this.extractor ?: return
        if (positionUs > 0 && (durationUs <= 0 || positionUs < durationUs)) {
            extractor.seekTo(positionUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            skipUntilUs = positionUs
        } else {
            extractor.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC)
            skipUntilUs = 0
        }
    }

    /** End of asset, or a decoder that gave up — restart from the top. */
    private fun restartLoop() {
        resumeUs = 0
        skipUntilUs = 0
        inputDone = false
        if (!isActive) return

        val codec = decoder ?: return
        runCatching {
            extractor?.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC)
            codec.flush()
        }.onFailure {
            Log.error("could not restart the loop: $it")
            failed = true
            deactivate()
        }
        synchronized(frameLock) { frameAvailable = false }
    }

    private companion object {
        const val MAX_STEPS_PER_TICK = 64
        const val DEQUEUE_TIMEOUT_US = 2_000L
        const val FRAME_WAIT_MS = 100L
    }
}

// MARK: - MediaFormat accessors
//
// MediaFormat throws on a missing key rather than returning a default, and the
// keys that matter here — rotation, duration, crop, frame rate — are all
// optional in practice.

internal fun MediaFormat.getIntegerOrNull(key: String): Int? =
    if (containsKey(key)) runCatching { getInteger(key) }.getOrNull() else null

internal fun MediaFormat.getLongOrNull(key: String): Long? =
    if (containsKey(key)) runCatching { getLong(key) }.getOrNull() else null

/** Muxers write this as an integer, encoders often as a float, and some files
 *  carry it as a string. */
internal fun MediaFormat.frameRateOrNull(): Float? {
    if (!containsKey(MediaFormat.KEY_FRAME_RATE)) return null
    runCatching { return getInteger(MediaFormat.KEY_FRAME_RATE).toFloat() }
    runCatching { return getFloat(MediaFormat.KEY_FRAME_RATE) }
    runCatching { return getString(MediaFormat.KEY_FRAME_RATE)?.toFloatOrNull() }
    return null
}
