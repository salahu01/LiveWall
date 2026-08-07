package com.fegno.livewall.importer

import android.content.Context
import com.fegno.livewall.support.Log
import java.io.File
import java.util.UUID

/**
 * On-disk store of converted wallpapers.
 *
 * Originals are never copied or modified — only the normalised output lives
 * here, so the library size stays proportional to what actually plays. On a
 * phone that is not a nicety: internal storage is the scarce resource, and a
 * user who imports a 4K clip should not silently lose a gigabyte to a copy of a
 * file they already have in Photos.
 */
class Library(context: Context) {

    val directory: File = File(context.filesDir, "library").apply { mkdirs() }
    private val indexFile = File(context.filesDir, "index.json")

    private val _items = ArrayList<WallpaperItem>()
    val items: List<WallpaperItem> get() = _items

    init {
        load()
    }

    // MARK: - Persistence

    private fun load() {
        val text = runCatching { indexFile.takeIf { it.isFile }?.readText() }.getOrNull()
        if (text.isNullOrBlank()) return

        val outcome = LibraryIndex.decode(text)

        // A file that is present but yields nothing usable is more likely a bug
        // or a bad migration than an empty library, and the next save() would
        // overwrite it moments later. Keep a copy before that happens.
        if (outcome.items.isEmpty() && outcome.dropped > 0) {
            val backup = File(indexFile.parentFile, "index.corrupt.json")
            runCatching { indexFile.copyTo(backup, overwrite = true) }
            Log.error("index unreadable — kept a copy at ${backup.name}")
        } else if (outcome.dropped > 0) {
            Log.error("skipped ${outcome.dropped} unreadable entries in the library index")
        }

        _items.clear()
        // Drop entries whose file vanished (manual delete, migration, a restore
        // that brought the index back without the media).
        _items.addAll(outcome.items.filter { file(it).isFile })
    }

    private fun save() {
        runCatching {
            val temporary = File(indexFile.parentFile, "index.json.tmp")
            temporary.writeText(LibraryIndex.encode(_items))
            if (!temporary.renameTo(indexFile)) {
                indexFile.writeText(LibraryIndex.encode(_items))
                temporary.delete()
            }
        }.onFailure { Log.error("could not write the library index: $it") }
    }

    // MARK: - Items

    fun file(item: WallpaperItem): File = File(directory, item.filename)

    fun destination(id: String): File = File(directory, "$id.mp4")

    fun newId(): String = UUID.randomUUID().toString()

    fun add(item: WallpaperItem) {
        _items.removeAll { it.id == item.id }
        _items.add(item)
        _items.sortByDescending { it.addedAt }
        save()
    }

    fun remove(item: WallpaperItem) {
        file(item).delete()
        _items.removeAll { it.id == item.id }
        save()
    }

    fun item(id: String?): WallpaperItem? = id?.let { wanted -> _items.firstOrNull { it.id == wanted } }

    val totalBytes: Long get() = _items.sumOf { it.byteCount }
}
