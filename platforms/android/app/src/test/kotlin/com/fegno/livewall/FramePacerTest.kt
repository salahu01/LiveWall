package com.fegno.livewall

import com.fegno.livewall.importer.FramePacer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class FramePacerTest {

    @Test
    fun `a source already at the target rate keeps every frame`() {
        val pacer = FramePacer(24)
        for (frame in 0 until 240) {
            val sourceUs = frame * 1_000_000L / 24
            assertNotNull("frame $frame was dropped", pacer.accept(sourceUs))
        }
        assertEquals(240, pacer.kept)
        assertEquals(0, pacer.dropped)
    }

    @Test
    fun `sixty into twenty-four keeps two frames in five`() {
        val pacer = FramePacer(24)
        val seconds = 10
        for (frame in 0 until 60 * seconds) {
            pacer.accept(frame * 1_000_000L / 60)
        }
        // 24 grid points a second over ten seconds, give or take the frame the
        // final grid point lands past.
        assertEquals(24 * seconds, pacer.kept)
        assertEquals(60 * seconds - 24 * seconds, pacer.dropped)
    }

    @Test
    fun `output timestamps are exactly on the grid and never accumulate error`() {
        val pacer = FramePacer(24)
        val frames = 24 * 600  // ten minutes
        val outputs = ArrayList<Long>(frames)
        for (frame in 0 until frames) {
            pacer.accept(frame * 1_000_000L / 24)?.let { outputs.add(it) }
        }
        assertEquals(frames, outputs.size)
        assertEquals(0L, outputs.first())
        // A pacer that accumulated a truncated 41_666 µs per frame would be
        // (41_666.67 - 41_666) × 14_400 ≈ 9.6 ms adrift by here, and a full
        // frame adrift over an hour. Exact arithmetic is not.
        assertEquals(599_958_333L, outputs.last())
        outputs.forEachIndexed { index, value ->
            assertEquals(index * 1_000_000L / 24, value)
        }
    }

    @Test
    fun `a source slower than the grid never emits the same instant twice`() {
        val pacer = FramePacer(30)
        val outputs = ArrayList<Long>()
        // 10 fps source against a 30 fps grid.
        for (frame in 0 until 100) {
            pacer.accept(frame * 1_000_000L / 10)?.let { outputs.add(it) }
        }
        assertEquals(outputs.size, outputs.distinct().size)
        assertEquals(0, pacer.dropped)
    }

    @Test
    fun `frames arriving between grid points are dropped`() {
        val pacer = FramePacer(10)
        assertNotNull(pacer.accept(0))
        // 10 fps grid: the next point is 100_000 µs.
        assertNull(pacer.accept(50_000))
        assertNotNull(pacer.accept(100_000))
        assertEquals(2, pacer.kept)
        assertEquals(1, pacer.dropped)
    }

    @Test
    fun `a zero or negative rate is clamped rather than dividing by zero`() {
        val pacer = FramePacer(0)
        assertNotNull(pacer.accept(0))
        assertNotNull(pacer.accept(1_000_000))
    }
}
