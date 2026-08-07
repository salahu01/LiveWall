package com.fegno.livewall

import com.fegno.livewall.importer.LibraryIndex
import com.fegno.livewall.importer.WallpaperItem
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LibraryIndexTest {

    private fun item(id: String, title: String = "Aurora") = WallpaperItem(
        id = id,
        title = title,
        filename = "$id.mp4",
        width = 1920,
        height = 1080,
        fps = 24,
        byteCount = 4_200_000,
        addedAt = 1_700_000_000_000,
        bitDepth = 10
    )

    @Test
    fun `a written index reads back unchanged`() {
        val items = listOf(item("a"), item("b", "Ember"))
        val outcome = LibraryIndex.decode(LibraryIndex.encode(items))
        assertEquals(0, outcome.dropped)
        assertEquals(items, outcome.items)
    }

    @Test
    fun `one bad row costs one row`() {
        // The whole point of decoding entry by entry: an index that fails as a
        // unit reads as an empty library, which looks to the user like every
        // wallpaper they imported has vanished.
        val text = """
            [
              {"id":"a","title":"Aurora","filename":"a.mp4","width":1920,"height":1080,"fps":24,"byteCount":10,"addedAt":1},
              {"id":"b","title":"Broken","filename":"b.mp4","height":1080,"fps":24},
              {"id":"c","title":"Ember","filename":"c.mp4","width":1280,"height":720,"fps":20,"byteCount":20,"addedAt":2}
            ]
        """.trimIndent()

        val outcome = LibraryIndex.decode(text)
        assertEquals(1, outcome.dropped)
        assertEquals(listOf("a", "c"), outcome.items.map { it.id })
    }

    @Test
    fun `an entry with no bit depth is read as eight-bit`() {
        // Indexes written before 10-bit support have no such field. A
        // non-optional addition would fail those entries, and they are exactly
        // the entries that predate the field.
        val text = """
            [{"id":"a","title":"Old","filename":"a.mp4","width":1920,"height":1080,"fps":24,"byteCount":10,"addedAt":1}]
        """.trimIndent()

        val outcome = LibraryIndex.decode(text)
        assertEquals(0, outcome.dropped)
        assertEquals(8, outcome.items.single().pixelBitDepth)
    }

    @Test
    fun `unknown fields from a future version do not fail the entry`() {
        val text = """
            [{"id":"a","title":"New","filename":"a.mp4","width":1920,"height":1080,"fps":24,
              "byteCount":10,"addedAt":1,"bitDepth":10,"colourSpace":"bt2020","playlistOrder":3}]
        """.trimIndent()

        val outcome = LibraryIndex.decode(text)
        assertEquals(0, outcome.dropped)
        assertEquals("New", outcome.items.single().title)
    }

    @Test
    fun `a file that is not JSON at all is reported as one drop and no items`() {
        // The caller distinguishes this from "an empty library" and keeps a copy
        // before overwriting it.
        val outcome = LibraryIndex.decode("this is not json")
        assertTrue(outcome.items.isEmpty())
        assertEquals(1, outcome.dropped)
    }

    @Test
    fun `an empty array is an empty library, not a corrupt one`() {
        val outcome = LibraryIndex.decode("[]")
        assertTrue(outcome.items.isEmpty())
        assertEquals(0, outcome.dropped)
    }

    @Test
    fun `nonsense geometry is refused rather than played`() {
        val text = """
            [{"id":"a","title":"Zero","filename":"a.mp4","width":0,"height":1080,"fps":24,"byteCount":1,"addedAt":1},
             {"id":"b","title":"NoFps","filename":"b.mp4","width":1920,"height":1080,"fps":0,"byteCount":1,"addedAt":1}]
        """.trimIndent()

        val outcome = LibraryIndex.decode(text)
        assertTrue(outcome.items.isEmpty())
        assertEquals(2, outcome.dropped)
    }

    @Test
    fun `a missing title falls back to the filename rather than dropping the row`() {
        val text = """
            [{"id":"a","filename":"a.mp4","width":1920,"height":1080,"fps":24,"byteCount":1,"addedAt":1}]
        """.trimIndent()

        val outcome = LibraryIndex.decode(text)
        assertEquals(0, outcome.dropped)
        assertEquals("a.mp4", outcome.items.single().title)
    }

    @Test
    fun `titles with quotes and newlines survive the round trip`() {
        val awkward = item("a", "A \"quoted\"\ntitle\twith\\escapes")
        val outcome = LibraryIndex.decode(LibraryIndex.encode(listOf(awkward)))
        assertEquals(0, outcome.dropped)
        assertEquals(awkward.title, outcome.items.single().title)
    }

    @Test
    fun `byte counts past two gigabytes survive`() {
        // Int would silently wrap here, and a 3 GB library would report as
        // negative.
        val large = item("a").copy(byteCount = 3_500_000_000)
        val outcome = LibraryIndex.decode(LibraryIndex.encode(listOf(large)))
        assertEquals(3_500_000_000L, outcome.items.single().byteCount)
    }

    @Test
    fun `size labels are human readable`() {
        assertEquals("4.2 MB", item("a").sizeLabel)
        assertEquals("512 B", item("a").copy(byteCount = 512).sizeLabel)
        assertEquals("1.5 GB", item("a").copy(byteCount = 1_500_000_000).sizeLabel)
    }

    @Test
    fun `the resolution label states everything that costs something`() {
        assertEquals("1920×1080 · 24 fps · 10-bit", item("a").resolutionLabel)
    }
}
