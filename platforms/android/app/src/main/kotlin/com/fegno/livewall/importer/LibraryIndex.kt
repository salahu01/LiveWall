package com.fegno.livewall.importer

import com.fegno.livewall.support.Json
import com.fegno.livewall.support.asList
import com.fegno.livewall.support.asMap
import com.fegno.livewall.support.int
import com.fegno.livewall.support.long
import com.fegno.livewall.support.string

/**
 * One converted wallpaper.
 *
 * [bitDepth] is nullable so an index written before 10-bit support still
 * decodes; absent means 8-bit, which is what those files are.
 */
data class WallpaperItem(
    val id: String,
    val title: String,
    val filename: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    val byteCount: Long,
    val addedAt: Long,
    val bitDepth: Int? = null
) {
    val pixelBitDepth: Int get() = bitDepth ?: 8

    val resolutionLabel: String get() = "$width×$height · $fps fps · $pixelBitDepth-bit"

    val sizeLabel: String
        get() {
            if (byteCount < 1000) return "$byteCount B"
            val units = arrayOf("kB", "MB", "GB")
            var value = byteCount / 1000.0
            var unit = 0
            while (value >= 1000 && unit < units.lastIndex) {
                value /= 1000.0
                unit++
            }
            return if (value >= 100) "${Math.round(value)} ${units[unit]}"
            else "%.1f %s".format(value, units[unit])
        }
}

/**
 * Reading and writing the on-disk index, with no filesystem and no Android
 * anywhere in it — so the leniency below is covered by tests that need no
 * device.
 */
object LibraryIndex {

    data class Outcome(val items: List<WallpaperItem>, val dropped: Int)

    /**
     * Decodes the index one entry at a time.
     *
     * Decoding the whole array as a unit is all-or-nothing: a single malformed
     * entry — one bad id, one field written by a future version — fails
     * everything and the library silently reads as empty, which looks to the
     * user like every wallpaper they imported has vanished. One bad row should
     * cost one row.
     */
    fun decode(text: String): Outcome {
        val rows = try {
            Json.parse(text).asList()
        } catch (_: Json.ParseException) {
            null
        } ?: return Outcome(emptyList(), 1)

        val items = ArrayList<WallpaperItem>(rows.size)
        var dropped = 0
        for (row in rows) {
            val item = decodeItem(row)
            if (item == null) dropped++ else items.add(item)
        }
        return Outcome(items, dropped)
    }

    private fun decodeItem(row: Any?): WallpaperItem? {
        val map = row.asMap() ?: return null
        val id = map.string("id")?.takeIf { it.isNotBlank() } ?: return null
        val filename = map.string("filename")?.takeIf { it.isNotBlank() } ?: return null
        val width = map.int("width") ?: return null
        val height = map.int("height") ?: return null
        val fps = map.int("fps") ?: return null
        if (width <= 0 || height <= 0 || fps <= 0) return null

        return WallpaperItem(
            id = id,
            title = map.string("title") ?: filename,
            filename = filename,
            width = width,
            height = height,
            fps = fps,
            byteCount = map.long("byteCount") ?: 0L,
            addedAt = map.long("addedAt") ?: 0L,
            bitDepth = map.int("bitDepth")
        )
    }

    fun encode(items: List<WallpaperItem>): String = Json.write(
        items.map { item ->
            linkedMapOf<String, Any?>(
                "id" to item.id,
                "title" to item.title,
                "filename" to item.filename,
                "width" to item.width,
                "height" to item.height,
                "fps" to item.fps,
                "byteCount" to item.byteCount,
                "addedAt" to item.addedAt,
                "bitDepth" to item.bitDepth
            )
        }
    )
}
