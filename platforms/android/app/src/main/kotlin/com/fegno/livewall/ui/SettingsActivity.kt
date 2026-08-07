package com.fegno.livewall.ui

import android.app.Activity
import android.app.AlertDialog
import android.app.WallpaperManager
import android.content.ComponentName
import android.content.Intent
import android.database.Cursor
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.fegno.livewall.app.LiveWallService
import com.fegno.livewall.importer.Library
import com.fegno.livewall.importer.Preset
import com.fegno.livewall.importer.Transcoder
import com.fegno.livewall.importer.WallpaperItem
import com.fegno.livewall.render.FitMode
import com.fegno.livewall.support.Log
import com.fegno.livewall.support.Settings
import com.fegno.livewall.support.currentDisplayTarget
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The whole interface.
 *
 * The macOS port is a status-bar menu and nothing else — no dock icon, no
 * window. Android has no equivalent surface, and a persistent notification would
 * be a worse trade than a screen the user opens twice: once to pick a video and
 * once to change their mind. So this is a plain scrolling column, built in code.
 *
 * Built in code rather than from a layout because it is the only screen in the
 * app, and pulling in AppCompat, Material or Compose to lay out nine rows would
 * add more to the APK than everything else here put together.
 *
 * The same activity is the launcher entry *and* the wallpaper picker's
 * "Settings" target, so it must never assume it owns the task.
 */
class SettingsActivity : Activity() {

    private lateinit var settings: Settings
    private lateinit var library: Library

    private val main = Handler(Looper.getMainLooper())
    private val importer = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "livewall-import").apply { priority = Thread.NORM_PRIORITY - 1 }
    }

    private lateinit var statusText: TextView
    private lateinit var libraryColumn: LinearLayout
    private lateinit var presetValue: TextView
    private lateinit var fitValue: TextView
    private lateinit var footerText: TextView

    private var importCancelled: AtomicBoolean? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        library = Library(this)
        setContentView(buildView())
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    override fun onDestroy() {
        importCancelled?.set(true)
        importer.shutdown()
        super.onDestroy()
    }

    // MARK: - View construction

    private fun buildView(): View {
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(24), dp(20), dp(32))
        }

        column.addView(heading("LiveWall"))
        column.addView(
            body(
                "A live wallpaper that stops rendering the moment nothing can " +
                    "see it — not pauses, stops: the decoder is released and the " +
                    "last frame stays on screen."
            )
        )

        statusText = body("").apply {
            setPadding(0, dp(16), 0, 0)
            setTypeface(typeface, Typeface.BOLD)
        }
        column.addView(statusText)

        column.addView(button("Set LiveWall as my wallpaper") { chooseWallpaper() }.apply {
            (layoutParams as LinearLayout.LayoutParams).topMargin = dp(12)
        })

        column.addView(sectionTitle("Wallpapers"))
        libraryColumn = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(libraryColumn)

        column.addView(button("Add video…") { pickVideo() })

        column.addView(sectionTitle("Import"))
        presetValue = body("")
        column.addView(
            row("Quality", presetValue) { choosePreset() }
        )
        column.addView(
            caption(
                "Videos are converted on import and only the converted file is " +
                    "ever played. Your original is never copied or modified."
            )
        )

        column.addView(sectionTitle("Playback"))
        fitValue = body("")
        column.addView(row("Scaling", fitValue) { chooseFitMode() })

        val chargingSwitch = Switch(this).apply {
            text = "Render only while charging"
            isChecked = settings.renderOnlyWhileCharging
            setPadding(0, dp(16), 0, dp(4))
            setOnCheckedChangeListener { _, checked ->
                settings.renderOnlyWhileCharging = checked
            }
        }
        column.addView(chargingSwitch)
        column.addView(
            caption(
                "Rendering already stops when the screen is off, when any app is " +
                    "in front, under Battery Saver, when the device is hot, and " +
                    "below 20% charge."
            )
        )

        footerText = caption("")
        column.addView(footerText)

        return ScrollView(this).apply {
            addView(column, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
    }

    // MARK: - State

    private fun refresh() {
        statusText.text = settings.status
        presetValue.text = "${settings.preset.name} · ${settings.preset.summary}"

        val fit = settings.fitMode
        val display = currentDisplayTarget(this)
        val selected = library.item(settings.selectedId)
        val effect = selected?.let {
            fit.effectDescription(it.width, it.height, display.pixelWidth, display.pixelHeight)
        }
        fitValue.text = if (effect != null) "${fit.title} — $effect" else "${fit.title} — ${fit.tradeoff}"

        rebuildLibrary()

        val total = library.totalBytes
        footerText.text = if (library.items.isEmpty()) {
            "No converted videos yet."
        } else {
            "${library.items.size} converted, ${bytes(total)} on this device."
        }
    }

    private fun rebuildLibrary() {
        libraryColumn.removeAllViews()

        val selectedId = settings.selectedId
        libraryColumn.addView(
            libraryRow(
                title = "Gradient",
                detail = "Procedural · no video · drifts at 10 fps",
                selected = selectedId == null || library.item(selectedId) == null,
                onSelect = { settings.selectedId = null; refresh() },
                onDelete = null
            )
        )

        for (item in library.items) {
            libraryColumn.addView(
                libraryRow(
                    title = item.title,
                    detail = "${item.resolutionLabel} · ${item.sizeLabel}",
                    selected = item.id == selectedId,
                    onSelect = { settings.selectedId = item.id; refresh() },
                    onDelete = { confirmDelete(item) }
                )
            )
        }
    }

    private fun confirmDelete(item: WallpaperItem) {
        AlertDialog.Builder(this)
            .setTitle("Remove ${item.title}?")
            .setMessage("The converted file is deleted. Your original is untouched.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Remove") { _, _ ->
                if (settings.selectedId == item.id) settings.selectedId = null
                library.remove(item)
                refresh()
            }
            .show()
    }

    // MARK: - Actions

    private fun chooseWallpaper() {
        val intent = Intent(WallpaperManager.ACTION_CHANGE_LIVE_WALLPAPER).putExtra(
            WallpaperManager.EXTRA_LIVE_WALLPAPER_COMPONENT,
            ComponentName(this, LiveWallService::class.java)
        )
        // Not every OEM launcher honours the direct-preview intent; the generic
        // picker is the fallback that always exists.
        if (intent.resolveActivity(packageManager) != null) {
            startActivity(intent)
        } else {
            startActivity(Intent(WallpaperManager.ACTION_LIVE_WALLPAPER_CHOOSER))
        }
    }

    private fun pickVideo() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("video/*")
        runCatching { startActivityForResult(intent, REQUEST_VIDEO) }
            .onFailure { toast("No app on this device can pick a video.") }
    }

    private fun choosePreset() {
        val presets = Preset.ALL
        val labels = presets.map { "${it.name}\n${it.summary}" }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Import quality")
            .setSingleChoiceItems(labels, presets.indexOfFirst { it.name == settings.preset.name }) { dialog, which ->
                settings.preset = presets[which]
                dialog.dismiss()
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun chooseFitMode() {
        val modes = FitMode.entries.toList()
        val display = currentDisplayTarget(this)
        val selected = library.item(settings.selectedId)

        val labels = modes.map { mode ->
            val effect = selected?.let {
                mode.effectDescription(it.width, it.height, display.pixelWidth, display.pixelHeight)
            }
            "${mode.title}\n${effect ?: mode.tradeoff}"
        }.toTypedArray()

        AlertDialog.Builder(this)
            .setTitle("Scaling")
            .setSingleChoiceItems(labels, modes.indexOf(settings.fitMode)) { dialog, which ->
                settings.fitMode = modes[which]
                dialog.dismiss()
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    @Deprecated("Superseded by the Activity Result APIs, which live in AndroidX.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_VIDEO || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        beginImport(uri)
    }

    // MARK: - Import

    private fun beginImport(uri: Uri) {
        val preset = settings.preset
        val display = currentDisplayTarget(this)
        val title = displayName(uri) ?: "Video"
        val id = library.newId()
        val destination = library.destination(id)

        val cancelled = AtomicBoolean(false)
        importCancelled = cancelled

        val progressBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            isIndeterminate = false
        }
        val dialogBody = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(16), dp(24), 0)
            addView(TextView(context).apply { text = "Converting $title…" })
            addView(progressBar)
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("Import")
            .setView(dialogBody)
            .setCancelable(false)
            .setNegativeButton("Cancel") { _, _ -> cancelled.set(true) }
            .create()
        dialog.show()

        importer.execute {
            try {
                val result = Transcoder.convert(
                    context = this,
                    source = uri,
                    destination = destination,
                    preset = preset,
                    display = display,
                    cancelled = { cancelled.get() },
                    progress = { fraction ->
                        main.post { progressBar.progress = (fraction * 100).toInt() }
                    }
                )

                val item = WallpaperItem(
                    id = id,
                    title = title.substringBeforeLast('.'),
                    filename = destination.name,
                    width = result.width,
                    height = result.height,
                    fps = result.fps,
                    byteCount = result.byteCount,
                    addedAt = System.currentTimeMillis(),
                    bitDepth = result.bitDepth
                )

                main.post {
                    dialog.dismiss()
                    library.add(item)
                    settings.selectedId = item.id
                    refresh()
                    toast("Added ${item.title} — ${item.resolutionLabel}")
                }
            } catch (_: Transcoder.CancelledException) {
                main.post {
                    dialog.dismiss()
                    toast("Import cancelled.")
                }
            } catch (error: Throwable) {
                Log.error("import failed: $error")
                main.post {
                    dialog.dismiss()
                    AlertDialog.Builder(this)
                        .setTitle("Could not import that video")
                        .setMessage(
                            error.message
                                ?: "It may be damaged, or in a format this device cannot decode."
                        )
                        .setPositiveButton("OK", null)
                        .show()
                }
            } finally {
                if (importCancelled === cancelled) importCancelled = null
            }
        }
    }

    private fun displayName(uri: Uri): String? {
        var cursor: Cursor? = null
        return try {
            cursor = contentResolver.query(
                uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
            )
            if (cursor != null && cursor.moveToFirst()) cursor.getString(0) else null
        } catch (_: Exception) {
            null
        } finally {
            cursor?.close()
        }
    }

    // MARK: - Small view helpers

    private fun heading(text: String) = TextView(this).apply {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
        setTypeface(typeface, Typeface.BOLD)
    }

    private fun sectionTitle(text: String) = TextView(this).apply {
        this.text = text.uppercase()
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
        setTypeface(typeface, Typeface.BOLD)
        alpha = 0.6f
        setPadding(0, dp(28), 0, dp(8))
    }

    private fun body(text: String) = TextView(this).apply {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
    }

    private fun caption(text: String) = TextView(this).apply {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        alpha = 0.6f
        setPadding(0, dp(8), 0, 0)
    }

    private fun button(text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text
        layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply {
            topMargin = dp(8)
        }
        setOnClickListener { onClick() }
    }

    private fun row(label: String, value: TextView, onClick: () -> Unit) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(12), 0, dp(12))
        isClickable = true
        addView(TextView(context).apply {
            text = label
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        })
        addView(value.apply { alpha = 0.7f })
        setOnClickListener { onClick() }
    }

    private fun libraryRow(
        title: String,
        detail: String,
        selected: Boolean,
        onSelect: () -> Unit,
        onDelete: (() -> Unit)?
    ) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(12), dp(12), dp(12), dp(12))
        if (selected) setBackgroundColor(Color.argb(28, 128, 128, 255))
        isClickable = true
        setOnClickListener { onSelect() }
        onDelete?.let { delete ->
            setOnLongClickListener { delete(); true }
        }

        addView(TextView(context).apply {
            text = if (selected) "●" else "○"
            setPadding(0, 0, dp(12), 0)
        })

        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)
            addView(TextView(context).apply {
                text = title
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            })
            addView(TextView(context).apply {
                text = detail
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                alpha = 0.6f
            })
        })

        if (onDelete != null) {
            addView(TextView(context).apply {
                text = "Hold to remove"
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
                alpha = 0.45f
            })
        }
    }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()

    private fun toast(message: String) =
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()

    private fun bytes(count: Long): String {
        if (count < 1000) return "$count B"
        val units = arrayOf("kB", "MB", "GB")
        var value = count / 1000.0
        var unit = 0
        while (value >= 1000 && unit < units.lastIndex) {
            value /= 1000.0
            unit++
        }
        return if (value >= 100) "${Math.round(value)} ${units[unit]}" else "%.1f %s".format(value, units[unit])
    }

    private companion object {
        const val REQUEST_VIDEO = 1001
    }
}
