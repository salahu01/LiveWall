package com.fegno.livewall

import android.content.Context
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import androidx.test.platform.app.InstrumentationRegistry
import com.fegno.livewall.importer.DisplayTarget
import com.fegno.livewall.importer.Preset
import com.fegno.livewall.importer.Transcoder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File

/**
 * The transcode path, on a real device.
 *
 * This is the one part of the app that a JVM test cannot reach: it is a hardware
 * encoder, a muxer and an EGL context, and all three belong to the device. What
 * is asserted here is not "it produced a file" but the specific guarantees the
 * playback path is built on — HEVC or AVC, exactly the paced frame rate, exactly
 * the computed dimensions, no audio track, and a real duration.
 *
 * Note for anyone reading a failure on an emulator: the codecs there are
 * software, so this is slow and the *timing* means nothing. The assertions are
 * all about the shape of the output, which is the same either way.
 */
class TranscodeTest {

    private lateinit var context: Context
    private lateinit var source: File
    private lateinit var destination: File

    @Before
    fun copySampleOutOfAssets() {
        context = InstrumentationRegistry.getInstrumentation().targetContext
        val assets = InstrumentationRegistry.getInstrumentation().context.assets

        source = File(context.cacheDir, "aurora-source.mp4")
        if (!source.isFile || source.length() == 0L) {
            assets.open("aurora.mp4").use { input ->
                source.outputStream().use { output -> input.copyTo(output) }
            }
        }
        destination = File(context.cacheDir, "aurora-converted.mp4")
        destination.delete()
    }

    @Test
    fun theOutputIsExactlyWhatThePlaybackPathExpects() {
        val display = DisplayTarget(1080, 2400, 60)
        val preset = Preset.ULTRA_LIGHT   // the cheapest to encode, same pipeline

        var lastFraction = -1.0
        val result = Transcoder.convert(
            context = context,
            source = Uri.fromFile(source),
            destination = destination,
            preset = preset,
            display = display,
            progress = { fraction ->
                assertTrue("progress went backwards", fraction >= lastFraction)
                assertTrue("progress out of range: $fraction", fraction in 0.0..1.0)
                lastFraction = fraction
            }
        )

        assertTrue("nothing was written", destination.isFile && destination.length() > 0)
        assertEquals(destination.length(), result.byteCount)

        // 20 fps divides a 60 Hz panel exactly, so pacing leaves it alone. A
        // frame landing between two refreshes is decoded and then dropped by the
        // compositor, which is the most wasteful thing this pipeline can do.
        assertEquals(20, result.fps)

        // Ultra Light caps the longest edge at 960 and the source is 1920×1080,
        // so 960 is the ceiling — not the guarantee. A device whose only HEVC
        // encoder tops out lower gets clamped further, which is the whole point
        // of the encoder-capability pass; the emulator's software HEVC encoder
        // stops at 512×512 and lands here legitimately.
        val longest = maxOf(result.width, result.height)
        assertTrue("longest edge was $longest", longest in 2..960)
        assertEquals(0, result.width % 2)
        assertEquals(0, result.height % 2)

        // Whatever the clamp did, it must not have changed the shape of the
        // picture — a stretched wallpaper is a bug, a smaller one is a trade.
        val sourceAspect = 1920.0 / 1080
        val outputAspect = result.width.toDouble() / result.height
        assertEquals("aspect ratio changed", sourceAspect, outputAspect, 0.02)

        assertTrue("bit depth was ${result.bitDepth}", result.bitDepth == 8 || result.bitDepth == 10)

        verifyContainer(result)
    }

    private fun verifyContainer(result: Transcoder.Result) {
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(destination.absolutePath)

            var videoTracks = 0
            var audioTracks = 0
            var videoFormat: MediaFormat? = null
            for (index in 0 until extractor.trackCount) {
                val format = extractor.getTrackFormat(index)
                val mime = format.getString(MediaFormat.KEY_MIME).orEmpty()
                when {
                    mime.startsWith("video/") -> {
                        videoTracks++
                        videoFormat = format
                    }
                    mime.startsWith("audio/") -> audioTracks++
                }
            }

            assertEquals("expected exactly one video track", 1, videoTracks)
            // No audio track means no audio graph at playback — nothing selects
            // it, so nothing is muxed.
            assertEquals("an audio track was carried through", 0, audioTracks)

            val format = videoFormat!!
            val mime = format.getString(MediaFormat.KEY_MIME)!!
            assertTrue(
                "playback assumes a hardware-decodable codec, got $mime",
                mime == MediaFormat.MIMETYPE_VIDEO_HEVC || mime == MediaFormat.MIMETYPE_VIDEO_AVC
            )

            assertEquals(result.width, format.getInteger(MediaFormat.KEY_WIDTH))
            assertEquals(result.height, format.getInteger(MediaFormat.KEY_HEIGHT))

            val durationUs = format.getLong(MediaFormat.KEY_DURATION)
            assertTrue("duration was $durationUs", durationUs > 1_000_000)

            // The real proof that the pacer stamped a constant grid: count the
            // samples and divide. Anything other than the requested rate means
            // the playback pump — which pulls one frame per tick at the rate in
            // the index — would play the clip at the wrong speed.
            extractor.selectTrack(indexOfVideoTrack(extractor))
            var samples = 0
            while (extractor.sampleTime >= 0) {
                samples++
                if (!extractor.advance()) break
            }
            val measuredFps = samples / (durationUs / 1_000_000.0)
            assertEquals(
                "counted $samples samples over ${durationUs / 1_000_000.0}s",
                result.fps.toDouble(), measuredFps, 1.0
            )
        } finally {
            extractor.release()
        }
    }

    private fun indexOfVideoTrack(extractor: MediaExtractor): Int {
        for (index in 0 until extractor.trackCount) {
            val mime = extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME).orEmpty()
            if (mime.startsWith("video/")) return index
        }
        error("no video track")
    }

    @Test
    fun cancellingStopsAndLeavesNothingBehind() {
        // A half-written file in the library directory would be indexed as a
        // wallpaper and fail at playback, so the transcoder deletes its output
        // on any path that is not success.
        var frames = 0
        val error = runCatching {
            Transcoder.convert(
                context = context,
                source = Uri.fromFile(source),
                destination = destination,
                preset = Preset.ULTRA_LIGHT,
                display = DisplayTarget(1080, 2400, 60),
                cancelled = { frames++ > 3 }
            )
        }.exceptionOrNull()

        assertTrue(
            "expected a CancelledException, got $error",
            error is Transcoder.CancelledException
        )
        assertTrue("a partial file was left behind", !destination.exists())
    }
}
