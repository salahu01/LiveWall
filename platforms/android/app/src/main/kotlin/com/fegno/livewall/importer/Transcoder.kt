package com.fegno.livewall.importer

import android.content.Context
import android.graphics.SurfaceTexture
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMuxer
import android.net.Uri
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.Surface
import com.fegno.livewall.render.EglCore
import com.fegno.livewall.render.FitMode
import com.fegno.livewall.render.OesQuadProgram
import com.fegno.livewall.render.frameRateOrNull
import com.fegno.livewall.render.getIntegerOrNull
import com.fegno.livewall.render.getLongOrNull
import com.fegno.livewall.support.Log
import java.io.File
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Normalises an arbitrary user video into a wallpaper-shaped asset.
 *
 * Import is mandatory, not an optimisation pass — the playback path only ever
 * sees files that came out of here. That is what makes the runtime numbers
 * predictable regardless of what the user picked out of their gallery.
 *
 * What each step is actually buying:
 *
 * - **Transcode to HEVC.** VP9 and AV1 are ordinary in files that arrive from
 *   the web, and on a phone whose SoC lacks the matching hardware block they
 *   fall back to software decode — tens of percent of a core, continuously, on a
 *   battery. HEVC goes through the media block.
 * - **Downscale.** A 4K source on a 1080p panel decodes four times the pixels
 *   nobody sees. Frame memory scales with this directly.
 * - **Cap the frame rate.** Ambient loops gain nothing above 24 fps and pay
 *   linearly for every frame above it — the one knob that costs.
 * - **Drop the audio track.** Nothing selects it, so nothing muxes it.
 * - **No B-frames.** The decoder needs no reorder buffer, which both shrinks its
 *   pool and removes decode latency.
 *
 * The pipeline is `MediaExtractor` → hardware decoder → external texture → GL →
 * hardware encoder's input surface → `MediaMuxer`. Frames never touch the CPU:
 * the scale, the rotation and the pixel-format conversion all happen in the
 * sampler. That is also why there is no `ffmpeg` here — it would add tens of
 * megabytes and a licensing question to an app whose entire point is being
 * small, and it would do the work on the CPU.
 */
object Transcoder {

    class TranscodeException(message: String) : Exception(message)

    class CancelledException : Exception("Conversion cancelled.")

    data class Result(
        val file: File,
        val width: Int,
        val height: Int,
        val fps: Int,
        val bitDepth: Int,
        val byteCount: Long
    )

    private const val HEVC = MediaFormat.MIMETYPE_VIDEO_HEVC
    private const val AVC = MediaFormat.MIMETYPE_VIDEO_AVC

    /**
     * Converts [source] into [destination]. Blocking — call it from a background
     * thread. [progress] is invoked with 0…1 on that same thread; [cancelled] is
     * polled between frames.
     */
    fun convert(
        context: Context,
        source: Uri,
        destination: File,
        preset: Preset,
        display: DisplayTarget,
        cancelled: () -> Boolean = { false },
        progress: (Double) -> Unit = {}
    ): Result {

        val extractor = MediaExtractor()
        var decoder: MediaCodec? = null
        var encoder: MediaCodec? = null
        var muxer: MediaMuxer? = null
        var egl: EglCore? = null
        var eglSurface: EGLSurface? = null
        var encoderSurface: Surface? = null
        var decoderSurface: Surface? = null
        var surfaceTexture: SurfaceTexture? = null
        var textureId = 0
        var frameThread: HandlerThread? = null
        var muxerStarted = false
        var succeeded = false

        val frameLock = java.lang.Object()
        var frameAvailable = false

        try {
            // MARK: Source

            context.contentResolver.openFileDescriptor(source, "r").use { descriptor
                ->
                if (descriptor == null) throw TranscodeException("Couldn't open that file.")
                extractor.setDataSource(descriptor.fileDescriptor)
            }

            var trackIndex = -1
            var sourceFormat: MediaFormat? = null
            for (index in 0 until extractor.trackCount) {
                val format = extractor.getTrackFormat(index)
                val mime = format.getString(MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("video/")) {
                    trackIndex = index
                    sourceFormat = format
                    break
                }
            }
            val inputFormat = sourceFormat
                ?: throw TranscodeException("That file has no video track.")
            extractor.selectTrack(trackIndex)

            val storedWidth = inputFormat.getInteger(MediaFormat.KEY_WIDTH)
            val storedHeight = inputFormat.getInteger(MediaFormat.KEY_HEIGHT)
            val rotation = ((inputFormat.getIntegerOrNull(MediaFormat.KEY_ROTATION) ?: 0) % 360 + 360) % 360
            val durationUs = inputFormat.getLongOrNull(MediaFormat.KEY_DURATION) ?: 0L
            val sourceFps = inputFormat.frameRateOrNull()?.takeIf { it > 0 }

            // Orientation-corrected source dimensions. A portrait clip stored
            // landscape with a 90° flag is a portrait clip, and sizing it as
            // landscape would fit it to the wrong edge.
            val oriented = if (rotation % 180 == 0)
                Dimensions(storedWidth, storedHeight)
            else
                Dimensions(storedHeight, storedWidth)

            // MARK: Output shape

            // Never upsample the frame rate past the source, and land on a rate
            // the panel can present evenly.
            val capped = sourceFps?.let { min(preset.fps, it.roundToInt()) } ?: preset.fps
            val fps = max(1, Sizing.pacedFps(capped, display.maximumFramesPerSecond))
            if (fps != capped) {
                Log.info(
                    "paced $capped fps down to $fps to divide a " +
                        "${display.maximumFramesPerSecond} Hz panel evenly"
                )
            }

            val requested = Sizing.outputSize(oriented, preset, display)

            // MARK: Encoder

            val (encoderMime, output) = chooseEncoder(requested)
            if (encoderMime != HEVC) {
                Log.info("importing as AVC rather than HEVC")
            }
            if (output != requested) {
                Log.info("encoder capped $requested to $output")
            }

            val wantsTenBit = preset.bitDepth >= 10 &&
                encoderMime == HEVC &&
                supportsProfile(HEVC, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10)

            // The EGL config has to be settled before the encoder is configured:
            // asking an encoder for Main10 and then feeding it an 8-bit surface
            // produces a 10-bit stream carrying 8-bit content, which costs
            // bitrate and fixes no banding. `EglCore` reports what it actually
            // got, and the library records that rather than what was asked for.
            val core = EglCore(recordable = true, preferTenBit = wantsTenBit)
            egl = core
            val tenBit = wantsTenBit && core.isTenBit
            if (wantsTenBit && !tenBit) {
                Log.info("no 10-bit EGL config here — encoding 8-bit")
            }

            val pixels = output.width.toDouble() * output.height
            val bitrate = (pixels * fps * preset.bitsPerPixel).toInt().coerceAtLeast(200_000)

            val encoderFormat = MediaFormat.createVideoFormat(encoderMime, output.width, output.height).apply {
                setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
                )
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                // Sparse keyframes: at these bitrates an I-frame every couple of
                // seconds eats a large share of the budget the moving content
                // wants. The only seek a wallpaper performs is the resume after
                // being hidden, which tolerates a longer run-up.
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, preset.keyframeSeconds.toInt())
                // No B-frames: the decoder needs no reorder buffer at playback,
                // and every frame being a reference frame is what lets the
                // playback pump be "one frame per tick" with no reordering
                // window of its own.
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                setInteger(MediaFormat.KEY_LATENCY, 1)
                if (tenBit) {
                    setInteger(
                        MediaFormat.KEY_PROFILE,
                        MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10
                    )
                }
            }

            val videoEncoder = MediaCodec.createEncoderByType(encoderMime)
            encoder = videoEncoder
            videoEncoder.configure(encoderFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val inputSurface = videoEncoder.createInputSurface()
            encoderSurface = inputSurface
            videoEncoder.start()

            eglSurface = core.createWindowSurface(inputSurface)
            core.makeCurrent(eglSurface)
            GLES20.glViewport(0, 0, output.width, output.height)

            // MARK: Decoder

            frameThread = HandlerThread("livewall-transcode-frames").apply { start() }
            val frameHandler = Handler(frameThread.looper)

            textureId = OesQuadProgram.createExternalTexture()
            val texture = SurfaceTexture(textureId)
            texture.setDefaultBufferSize(oriented.width, oriented.height)
            texture.setOnFrameAvailableListener({
                synchronized(frameLock) {
                    frameAvailable = true
                    frameLock.notifyAll()
                }
            }, frameHandler)
            surfaceTexture = texture

            val sourceSurface = Surface(texture)
            decoderSurface = sourceSurface

            val decoderMime = inputFormat.getString(MediaFormat.KEY_MIME)!!
            val videoDecoder = MediaCodec.createDecoderByType(decoderMime)
            decoder = videoDecoder
            videoDecoder.configure(inputFormat, sourceSurface, null, 0)
            videoDecoder.start()

            // MARK: Muxer

            val fileMuxer = MediaMuxer(
                destination.absolutePath,
                MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4
            )
            muxer = fileMuxer
            var muxerTrack = -1

            // MARK: The pump
            //
            // Setting a frame rate on the encoder resizes nothing and retimes
            // nothing — it is a hint. The frame grid has to be enforced here, by
            // dropping source frames that fall between grid points and stamping
            // the survivors onto exact 1/fps boundaries. This matters beyond
            // tidiness: the playback path pulls exactly one frame per tick at the
            // rate recorded in the library, so a file whose real rate differs
            // from its recorded rate plays at the wrong speed.
            val pacer = FramePacer(fps)
            val program = OesQuadProgram()
            val texMatrix = FloatArray(16)
            val decoderInfo = MediaCodec.BufferInfo()
            val encoderInfo = MediaCodec.BufferInfo()

            var inputDone = false
            var decoderDone = false
            var encoderDone = false
            var contentWidth = oriented.width
            var contentHeight = oriented.height

            fun drainEncoder() {
                while (true) {
                    val index = videoEncoder.dequeueOutputBuffer(encoderInfo, 0)
                    when {
                        index == MediaCodec.INFO_TRY_AGAIN_LATER -> return

                        index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            check(!muxerStarted) { "encoder changed format twice" }
                            muxerTrack = fileMuxer.addTrack(videoEncoder.outputFormat)
                            fileMuxer.start()
                            muxerStarted = true
                        }

                        index < 0 -> return

                        else -> {
                            val buffer = videoEncoder.getOutputBuffer(index)
                            // Codec config bytes go into the track format, not
                            // into the stream; writing them as a sample produces
                            // a file that plays everywhere except where it
                            // matters.
                            if (encoderInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                                encoderInfo.size = 0
                            }
                            if (encoderInfo.size > 0 && muxerStarted && buffer != null) {
                                buffer.position(encoderInfo.offset)
                                buffer.limit(encoderInfo.offset + encoderInfo.size)
                                fileMuxer.writeSampleData(muxerTrack, buffer, encoderInfo)
                            }
                            videoEncoder.releaseOutputBuffer(index, false)
                            if (encoderInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                                encoderDone = true
                                return
                            }
                        }
                    }
                }
            }

            fun awaitFrame(): Boolean {
                synchronized(frameLock) {
                    val deadline = SystemClock.uptimeMillis() + FRAME_WAIT_MS
                    while (!frameAvailable) {
                        val remaining = deadline - SystemClock.uptimeMillis()
                        if (remaining <= 0) return false
                        frameLock.wait(remaining)
                    }
                    frameAvailable = false
                }
                return true
            }

            val totalUs = max(durationUs, 1L)
            var lastReported = -1.0

            while (!encoderDone) {
                if (cancelled()) throw CancelledException()

                // Feed the decoder.
                if (!inputDone) {
                    val index = videoDecoder.dequeueInputBuffer(TIMEOUT_US)
                    if (index >= 0) {
                        val buffer = videoDecoder.getInputBuffer(index)
                        val size = if (buffer == null) -1 else extractor.readSampleData(buffer, 0)
                        if (size < 0) {
                            videoDecoder.queueInputBuffer(
                                index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM
                            )
                            inputDone = true
                        } else {
                            videoDecoder.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }

                // Drain the decoder, retime, draw.
                if (!decoderDone) {
                    val index = videoDecoder.dequeueOutputBuffer(decoderInfo, TIMEOUT_US)
                    when {
                        index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val changed = videoDecoder.outputFormat
                            val width = changed.getIntegerOrNull(MediaFormat.KEY_WIDTH)
                            val height = changed.getIntegerOrNull(MediaFormat.KEY_HEIGHT)
                            if (width != null && height != null && width > 0 && height > 0) {
                                if (rotation % 180 == 0) {
                                    contentWidth = width
                                    contentHeight = height
                                } else {
                                    contentWidth = height
                                    contentHeight = width
                                }
                            }
                        }

                        index >= 0 -> {
                            val endOfStream =
                                decoderInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                            val outputUs = if (!endOfStream && decoderInfo.size > 0)
                                pacer.accept(decoderInfo.presentationTimeUs) else null

                            videoDecoder.releaseOutputBuffer(index, outputUs != null)

                            if (outputUs != null) {
                                if (!awaitFrame()) {
                                    throw TranscodeException("The decoder stopped producing frames.")
                                }
                                texture.updateTexImage()
                                texture.getTransformMatrix(texMatrix)

                                core.makeCurrent(eglSurface)
                                GLES20.glClearColor(0f, 0f, 0f, 1f)
                                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

                                // FILL rather than FIT: the output size preserves
                                // the source's aspect ratio, so the two agree to
                                // within the half pixel that rounding to even
                                // dimensions costs — and half a pixel of crop is
                                // better than half a pixel of black bar baked
                                // into the file forever.
                                val scale = FitMode.FILL.scale(
                                    contentWidth, contentHeight, output.width, output.height
                                )
                                program.draw(
                                    textureId,
                                    OesQuadProgram.rotatedTexMatrix(texMatrix, rotation),
                                    scale[0], scale[1]
                                )
                                core.setPresentationTime(eglSurface, outputUs * 1_000L)
                                core.swapBuffers(eglSurface)
                            }

                            val fraction = (decoderInfo.presentationTimeUs.toDouble() / totalUs)
                                .coerceIn(0.0, 1.0)
                            // The UI cannot show more than a hundred steps and
                            // each report crosses a thread.
                            if (fraction - lastReported >= 0.01) {
                                lastReported = fraction
                                progress(fraction)
                            }

                            if (endOfStream) {
                                decoderDone = true
                                videoEncoder.signalEndOfInputStream()
                            }
                        }
                    }
                }

                drainEncoder()
            }

            program.release()

            if (!muxerStarted) {
                throw TranscodeException("The encoder produced nothing.")
            }

            progress(1.0)
            succeeded = true

            val byteCount = destination.length()
            Log.info(
                "converted to ${output.width}×${output.height} @ ${fps}fps " +
                    "${if (tenBit) 10 else 8}-bit, kept ${pacer.kept} dropped ${pacer.dropped}, " +
                    "${byteCount / 1024} KB"
            )

            return Result(
                file = destination,
                width = output.width,
                height = output.height,
                fps = fps,
                bitDepth = if (tenBit) 10 else 8,
                byteCount = byteCount
            )
        } finally {
            // Order matters: the muxer has to stop before its file is usable,
            // and the codecs have to stop before the surfaces they write into go
            // away.
            if (muxerStarted) runCatching { muxer?.stop() }
            runCatching { muxer?.release() }

            runCatching { decoder?.stop() }
            runCatching { decoder?.release() }
            runCatching { encoder?.stop() }
            runCatching { encoder?.release() }

            decoderSurface?.release()
            surfaceTexture?.setOnFrameAvailableListener(null)
            surfaceTexture?.release()
            encoderSurface?.release()

            if (textureId != 0 && egl != null) {
                runCatching { GLES20.glDeleteTextures(1, intArrayOf(textureId), 0) }
            }
            egl?.releaseSurface(eglSurface)
            egl?.release()

            frameThread?.quitSafely()
            extractor.release()

            if (!succeeded) destination.delete()
        }
    }

    // MARK: - Encoder capabilities

    /**
     * Picks the codec to encode with, and the size it will actually accept.
     *
     * HEVC is the target — it is what the playback path is tuned around and what
     * keeps the bitrate down. But unlike macOS it is not a given. HEVC *decode*
     * has been mandatory since Android 5; HEVC *encode* has not, and where an
     * encoder does exist it is sometimes a token one: Android's own software
     * fallback, `c2.android.hevc.encoder`, tops out at 512×512.
     *
     * Shrinking a 960p import to 512p to keep the nicer codec is the wrong
     * trade — resolution is the thing that survives to the screen, and AVC at
     * the full size is hardware-decoded just the same. So the rule is: take
     * HEVC unless AVC can hold materially more picture.
     */
    private fun chooseEncoder(requested: Dimensions): Pair<String, Dimensions> {
        val hevc = if (hasEncoder(HEVC)) fitToEncoder(HEVC, requested) else null
        val avc = if (hasEncoder(AVC)) fitToEncoder(AVC, requested) else null

        if (hevc == null && avc == null) {
            throw TranscodeException("This device has no H.264 or HEVC video encoder.")
        }
        if (hevc == null) return AVC to avc!!
        if (avc == null) return HEVC to hevc

        val hevcPixels = hevc.width.toLong() * hevc.height
        val avcPixels = avc.width.toLong() * avc.height
        // A tenth more picture is the threshold: below that the codec is worth
        // more than the pixels.
        return if (avcPixels > hevcPixels * 11 / 10) AVC to avc else HEVC to hevc
    }

    private fun hasEncoder(mime: String): Boolean = encoderCapabilities(mime) != null

    private fun supportsProfile(mime: String, profile: Int): Boolean {
        val capabilities = encoderCapabilities(mime) ?: return false
        return capabilities.profileLevels.any { it.profile == profile }
    }

    private fun encoderCapabilities(mime: String): MediaCodecInfo.CodecCapabilities? {
        val codecs = MediaCodecList(MediaCodecList.REGULAR_CODECS)
        // Hardware first: a software HEVC encoder exists on some devices and
        // would turn a one-minute import into a ten-minute one.
        val sorted = codecs.codecInfos
            .filter { it.isEncoder && it.supportedTypes.any { type -> type.equals(mime, true) } }
            .sortedByDescending { it.isHardwareAccelerated }
        for (info in sorted) {
            runCatching { return info.getCapabilitiesForType(mime) }
        }
        return null
    }

    /**
     * Clamps [requested] into something the encoder will actually accept.
     *
     * Phones are far less uniform here than Macs: an encoder that tops out at
     * 1920×1088, or that insists on a multiple of 16, is ordinary. Without this
     * a 4K import fails at `configure` with an exception whose message names
     * neither the limit nor the dimension that broke it.
     */
    internal fun fitToEncoder(mime: String, requested: Dimensions): Dimensions {
        val video = encoderCapabilities(mime)?.videoCapabilities ?: return requested

        val widthRange = video.supportedWidths
        val heightRange = video.supportedHeights
        val widthAlignment = max(2, video.widthAlignment)
        val heightAlignment = max(2, video.heightAlignment)

        // Scale down uniformly until both edges are inside their ranges, so the
        // aspect ratio survives the clamp.
        var scale = 1.0
        if (requested.width > widthRange.upper) {
            scale = min(scale, widthRange.upper.toDouble() / requested.width)
        }
        if (requested.height > heightRange.upper) {
            scale = min(scale, heightRange.upper.toDouble() / requested.height)
        }

        fun align(value: Int, alignment: Int, lower: Int, upper: Int): Int {
            val floored = (value / alignment) * alignment
            return floored.coerceIn(
                (lower + alignment - 1) / alignment * alignment,
                (upper / alignment) * alignment
            )
        }

        val width = align(
            (requested.width * scale).roundToInt(), widthAlignment,
            widthRange.lower, widthRange.upper
        )
        val height = align(
            (requested.height * scale).roundToInt(), heightAlignment,
            heightRange.lower, heightRange.upper
        )
        return Dimensions(max(2, width), max(2, height))
    }

    private const val TIMEOUT_US = 5_000L
    private const val FRAME_WAIT_MS = 2_000L
}
