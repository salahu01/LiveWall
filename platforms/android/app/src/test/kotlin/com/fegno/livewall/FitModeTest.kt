package com.fegno.livewall

import com.fegno.livewall.render.FitMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class FitModeTest {

    // A 20:9 phone panel in portrait, and a 16:9 landscape clip — the pairing
    // almost every import produces.
    private val panelWidth = 1080
    private val panelHeight = 2400

    // MARK: - Geometry

    @Test
    fun `stretch always covers exactly`() {
        val scale = FitMode.STRETCH.scale(1920, 1080, panelWidth, panelHeight)
        assertEquals(1f, scale[0], 0f)
        assertEquals(1f, scale[1], 0f)
    }

    @Test
    fun `fill scales past the viewport on the overflowing axis`() {
        val scale = FitMode.FILL.scale(1920, 1080, panelWidth, panelHeight)
        // A landscape clip on a portrait panel has to grow horizontally until
        // the height is covered; the rasteriser clips the rest.
        assertTrue("expected an x scale above 1, got ${scale[0]}", scale[0] > 1f)
        assertEquals(1f, scale[1], 0.0001f)
    }

    @Test
    fun `fit scales inside the viewport on the same axis`() {
        val scale = FitMode.FIT.scale(1920, 1080, panelWidth, panelHeight)
        assertEquals(1f, scale[0], 0.0001f)
        assertTrue("expected a y scale below 1, got ${scale[1]}", scale[1] < 1f)
    }

    @Test
    fun `fill and fit are reciprocal on the axis they act on`() {
        val fill = FitMode.FILL.scale(1920, 1080, panelWidth, panelHeight)
        val fit = FitMode.FIT.scale(1920, 1080, panelWidth, panelHeight)
        assertEquals(1.0, (fill[0] * fit[1]).toDouble(), 0.001)
    }

    @Test
    fun `matching aspect ratios are identity in every mode`() {
        for (mode in FitMode.entries) {
            val scale = mode.scale(1080, 2400, panelWidth, panelHeight)
            assertEquals("${mode.title} x", 1f, scale[0], 0.0001f)
            assertEquals("${mode.title} y", 1f, scale[1], 0.0001f)
        }
    }

    @Test
    fun `degenerate sizes fall back to identity rather than dividing by zero`() {
        for (mode in FitMode.entries) {
            val scale = mode.scale(0, 0, panelWidth, panelHeight)
            assertEquals(1f, scale[0], 0f)
            assertEquals(1f, scale[1], 0f)
            assertTrue(scale.all { it.isFinite() })
        }
    }

    // MARK: - What it tells the user

    @Test
    fun `a matching aspect ratio has nothing to say`() {
        for (mode in FitMode.entries) {
            assertNull(mode.effectDescription(1080, 2400, panelWidth, panelHeight))
        }
    }

    @Test
    fun `fill names the axis it crops`() {
        // 16:9 content on a 20:9 portrait panel is far wider than the panel, so
        // filling crops the width.
        val description = FitMode.FILL.effectDescription(1920, 1080, panelWidth, panelHeight)
        assertNotNull(description)
        assertTrue(description!!, description.contains("width"))
        assertTrue(description, description.startsWith("Crops"))
    }

    @Test
    fun `fit names the bars it adds`() {
        val description = FitMode.FIT.effectDescription(1920, 1080, panelWidth, panelHeight)
        assertNotNull(description)
        assertTrue(description!!, description.contains("above and below"))
    }

    @Test
    fun `stretch admits which way it distorts`() {
        val squeezed = FitMode.STRETCH.effectDescription(1920, 1080, panelWidth, panelHeight)
        assertNotNull(squeezed)
        assertTrue(squeezed!!, squeezed.startsWith("Squeezes"))

        val stretched = FitMode.STRETCH.effectDescription(1080, 1920, 2400, 1080)
        assertNotNull(stretched)
        assertTrue(stretched!!, stretched.startsWith("Stretches"))
    }

    @Test
    fun `the percentage matches the geometry it describes`() {
        // 1920x1080 on 1080x2400: content aspect 1.778, panel aspect 0.45,
        // ratio 3.95 — filling keeps 1/3.95 of the width, so it crops 75%.
        val description = FitMode.FILL.effectDescription(1920, 1080, panelWidth, panelHeight)!!
        val percent = Regex("(\\d+)%").find(description)!!.groupValues[1].toInt()

        val ratio = (1920.0 / 1080) / (panelWidth.toDouble() / panelHeight)
        val expected = Math.round((1 - 1 / ratio) * 100).toInt()
        assertTrue("$description said $percent%, geometry says $expected%", abs(percent - expected) <= 1)
    }

    @Test
    fun `a sub-half-percent difference is not worth mentioning`() {
        // 1000x1000 against 1002x1000 — a real difference, below the threshold
        // where any mode does something a person can see.
        assertNull(FitMode.FILL.effectDescription(1000, 1000, 1002, 1000))
    }

    // MARK: - Persistence

    @Test
    fun `keys round-trip and an unknown key falls back to the default`() {
        for (mode in FitMode.entries) {
            assertEquals(mode, FitMode.fromKey(mode.key))
        }
        assertEquals(FitMode.DEFAULT, FitMode.fromKey(null))
        assertEquals(FitMode.DEFAULT, FitMode.fromKey("cover"))
        assertEquals(FitMode.FILL, FitMode.DEFAULT)
    }
}
