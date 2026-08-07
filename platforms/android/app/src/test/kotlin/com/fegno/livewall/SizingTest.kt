package com.fegno.livewall

import com.fegno.livewall.importer.Dimensions
import com.fegno.livewall.importer.DisplayTarget
import com.fegno.livewall.importer.Preset
import com.fegno.livewall.importer.Sizing
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.max

class SizingTest {

    private val phone = DisplayTarget(1080, 2400, 120)
    private val sixtyHertz = DisplayTarget(1080, 2340, 60)

    // MARK: - pacedFps

    @Test
    fun `twenty-four divides one hundred and twenty exactly`() {
        assertEquals(24, Sizing.pacedFps(preferred = 24, refresh = 120))
    }

    @Test
    fun `twenty-four on a sixty hertz panel snaps down to twenty`() {
        // 60 % 24 != 0, and a frame landing between two refreshes is decoded and
        // then dropped by the compositor — the most wasteful thing this pipeline
        // can do.
        assertEquals(20, Sizing.pacedFps(preferred = 24, refresh = 60))
    }

    @Test
    fun `twenty-four on a ninety hertz panel snaps to eighteen`() {
        // 90 Hz is ordinary on mid-range phones and divides neither 24 nor 30.
        assertEquals(18, Sizing.pacedFps(preferred = 24, refresh = 90))
    }

    @Test
    fun `a rate that already divides the panel is left alone`() {
        assertEquals(20, Sizing.pacedFps(preferred = 20, refresh = 60))
        assertEquals(30, Sizing.pacedFps(preferred = 30, refresh = 90))
        assertEquals(24, Sizing.pacedFps(preferred = 24, refresh = 144))
    }

    @Test
    fun `the result never exceeds either input`() {
        for (preferred in 1..60) {
            for (refresh in listOf(60, 90, 120, 144)) {
                val paced = Sizing.pacedFps(preferred, refresh)
                assertTrue("$paced > $preferred", paced <= preferred)
            }
        }
    }

    @Test
    fun `nothing sensible below twelve fps is worth snapping to`() {
        // 13 divides nothing here; rather than walk down to 1 fps the rule gives
        // up and keeps the preferred rate.
        assertEquals(13, Sizing.pacedFps(preferred = 13, refresh = 100))
    }

    @Test
    fun `degenerate inputs do not divide by zero`() {
        assertEquals(1, Sizing.pacedFps(preferred = 0, refresh = 60))
        assertEquals(24, Sizing.pacedFps(preferred = 24, refresh = 0))
    }

    // MARK: - outputSize

    @Test
    fun `a fixed max edge fits the source inside it`() {
        val output = Sizing.outputSize(Dimensions(3840, 2160), Preset.BALANCED, phone)
        assertEquals(1920, output.width)
        assertEquals(1080, output.height)
    }

    @Test
    fun `a source smaller than the max edge is never upscaled`() {
        // The source has no detail past its own resolution; inventing pixels
        // only costs memory.
        val output = Sizing.outputSize(Dimensions(1280, 720), Preset.BALANCED, phone)
        assertEquals(Dimensions(1280, 720), output)
    }

    @Test
    fun `the native preset covers the panel rather than fitting inside it`() {
        // A portrait 1080x1920 source on a 1080x2400 panel: fitting would leave
        // it upscaled at playback, which was the single largest quality loss in
        // the pipeline. Covering keeps it at 1:1 — capped there, never above.
        val output = Sizing.outputSize(Dimensions(1080, 1920), Preset.NATIVE, phone)
        assertEquals(Dimensions(1080, 1920), output)
    }

    @Test
    fun `the native preset still never scales above one to one`() {
        val output = Sizing.outputSize(Dimensions(640, 360), Preset.NATIVE, phone)
        assertEquals(Dimensions(640, 360), output)
    }

    @Test
    fun `the native preset downscales an oversized source to cover the panel`() {
        val output = Sizing.outputSize(Dimensions(3840, 2160), Preset.NATIVE, phone)
        // Cover needs max(1080/3840, 2400/2160) = 1.111 → capped to 1.0, so a
        // 4K landscape source on a portrait panel stays 4K. That is the honest
        // answer: nothing smaller covers 2400 px of height.
        assertEquals(Dimensions(3840, 2160), output)
    }

    @Test
    fun `output dimensions are always even`() {
        val sources = listOf(
            Dimensions(1919, 1079),
            Dimensions(1001, 563),
            Dimensions(3, 7),
            Dimensions(4097, 2161)
        )
        for (preset in Preset.ALL) {
            for (source in sources) {
                val output = Sizing.outputSize(source, preset, sixtyHertz)
                assertEquals("width of $source under ${preset.name}", 0, output.width % 2)
                assertEquals("height of $source under ${preset.name}", 0, output.height % 2)
                assertTrue(output.width >= 2 && output.height >= 2)
            }
        }
    }

    @Test
    fun `aspect ratio survives the downscale`() {
        val source = Dimensions(3840, 1600)
        val output = Sizing.outputSize(source, Preset.ULTRA_LIGHT, phone)
        val sourceAspect = source.width.toDouble() / source.height
        val outputAspect = output.width.toDouble() / output.height
        // Rounding to even dimensions is the only permitted drift.
        assertTrue(
            "$source -> $output changed the aspect ratio",
            abs(sourceAspect - outputAspect) < 0.01
        )
    }

    @Test
    fun `a degenerate source falls back to something playable`() {
        val output = Sizing.outputSize(Dimensions(0, 0), Preset.BALANCED, phone)
        assertTrue(output.width >= 2 && output.height >= 2)
        assertEquals(0, output.width % 2)
        assertEquals(0, output.height % 2)
    }

    // MARK: - orient

    @Test
    fun `a quarter turn swaps the edges and a half turn does not`() {
        val landscape = Dimensions(1920, 1080)
        assertEquals(Dimensions(1920, 1080), Sizing.orient(landscape, 0))
        assertEquals(Dimensions(1080, 1920), Sizing.orient(landscape, 90))
        assertEquals(Dimensions(1920, 1080), Sizing.orient(landscape, 180))
        assertEquals(Dimensions(1080, 1920), Sizing.orient(landscape, 270))
    }

    @Test
    fun `orienting past a full revolution wraps`() {
        val landscape = Dimensions(1920, 1080)
        assertEquals(Sizing.orient(landscape, 90), Sizing.orient(landscape, 450))
        assertEquals(Sizing.orient(landscape, 90), Sizing.orient(landscape, -270))
        assertEquals(Sizing.orient(landscape, 0), Sizing.orient(landscape, 360))
    }

    @Test
    fun `a turned clip is sized against the edge it actually has`() {
        // The bug this guards: a 1920×1080 source turned 90° is a portrait clip.
        // Sizing it as landscape fits 1920 to the maxEdge and leaves the real
        // long edge — the 1080 that became the height — fitted to nothing.
        val turned = Sizing.orient(Dimensions(1920, 1080), 90)
        val output = Sizing.outputSize(turned, Preset.ULTRA_LIGHT, phone)
        assertEquals("the long edge should be the one capped", 960, max(output.width, output.height))
        assertTrue("the output should be portrait", output.height > output.width)
    }
}
