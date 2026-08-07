package com.fegno.livewall

import com.fegno.livewall.importer.ImportOptions
import com.fegno.livewall.importer.Preset
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ImportOptionsTest {

    // MARK: - Frame rate

    @Test
    fun `no opinion falls back to the preset's rate`() {
        assertEquals(24, ImportOptions().preferredFps(Preset.BALANCED))
        assertEquals(20, ImportOptions().preferredFps(Preset.ULTRA_LIGHT))
    }

    @Test
    fun `a chosen rate wins over the preset`() {
        assertEquals(30, ImportOptions(fps = 30).preferredFps(Preset.BALANCED))
        // Below the preset too — the point is the user's number, not a ceiling
        // raise.
        assertEquals(15, ImportOptions(fps = 15).preferredFps(Preset.NATIVE))
    }

    @Test
    fun `a rate outside the offered range is clamped, not rejected`() {
        // A typo in a free-entry box must not reach the encoder's bitrate maths.
        assertEquals(ImportOptions.MAX_FPS, ImportOptions(fps = 9000).preferredFps(Preset.BALANCED))
        assertEquals(ImportOptions.MIN_FPS, ImportOptions(fps = 1).preferredFps(Preset.BALANCED))
        assertEquals(ImportOptions.MIN_FPS, ImportOptions(fps = -30).preferredFps(Preset.BALANCED))
    }

    @Test
    fun `the floor is at or above the pacer's snapping floor`() {
        // Sizing.pacedFps gives up below 12 and returns the request unsnapped.
        // Offering a rate under that would hand back the judder the rest of the
        // pipeline exists to avoid.
        assertTrue(ImportOptions.MIN_FPS >= 12)
    }

    // MARK: - Rotation

    @Test
    fun `rotation is additive on top of the source flag`() {
        // A clip already stored with a 90° flag, turned another 90° by the user,
        // is a 180° clip — which is what pressing rotate twice looks like.
        assertEquals(180, ImportOptions(rotationDegrees = 90).effectiveRotation(90))
        assertEquals(90, ImportOptions(rotationDegrees = 90).effectiveRotation(0))
        assertEquals(90, ImportOptions(rotationDegrees = 0).effectiveRotation(90))
    }

    @Test
    fun `a full turn comes back to zero rather than to three hundred and sixty`() {
        assertEquals(0, ImportOptions(rotationDegrees = 270).effectiveRotation(90))
        assertEquals(0, ImportOptions(rotationDegrees = 180).effectiveRotation(180))
    }

    @Test
    fun `a negative or out-of-range source flag does not produce a negative angle`() {
        // KEY_ROTATION is documented as a multiple of 90, but it is read off a
        // file this app did not write.
        assertEquals(270, ImportOptions(rotationDegrees = 0).effectiveRotation(-90))
        assertEquals(90, ImportOptions(rotationDegrees = 0).effectiveRotation(450))
        assertEquals(0, ImportOptions(rotationDegrees = -90).effectiveRotation(90))
    }

    @Test
    fun `every offered rotation is a quarter turn inside one revolution`() {
        assertEquals(listOf(0, 90, 180, 270), ImportOptions.ROTATIONS)
        for (degrees in ImportOptions.ROTATIONS) {
            assertEquals(0, degrees % 90)
            assertEquals(degrees, ImportOptions.normalised(degrees))
        }
    }

    @Test
    fun `the default asks for nothing`() {
        assertEquals(null, ImportOptions.DEFAULT.fps)
        assertEquals(0, ImportOptions.DEFAULT.rotationDegrees)
        // So an unchanged import still lands exactly where it did before.
        assertEquals(
            Preset.BALANCED.fps,
            ImportOptions.DEFAULT.preferredFps(Preset.BALANCED)
        )
        assertEquals(37, ImportOptions.DEFAULT.effectiveRotation(37))
    }
}
