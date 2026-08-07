package com.fegno.livewall.support

import android.content.Context
import android.content.SharedPreferences
import com.fegno.livewall.importer.Preset
import com.fegno.livewall.render.FitMode

/**
 * Everything the settings screen and the wallpaper service both need to see.
 *
 * They are separate components in the same process with no binder between them,
 * so `SharedPreferences` is the whole IPC story: the activity writes, the
 * service's change listener wakes and re-evaluates. That is also why [status]
 * lives here — it is the one value that flows the other way, from the engine
 * back to the UI, and giving it its own channel would mean a service binding
 * that exists solely to render one line of text.
 */
class Settings(context: Context) {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("livewall", Context.MODE_PRIVATE)

    var selectedId: String?
        get() = prefs.getString(KEY_SELECTED, null)
        set(value) = prefs.edit().apply {
            if (value == null) remove(KEY_SELECTED) else putString(KEY_SELECTED, value)
        }.apply()

    var preset: Preset
        get() = Preset.named(prefs.getString(KEY_PRESET, null))
        set(value) = prefs.edit().putString(KEY_PRESET, value.name).apply()

    var fitMode: FitMode
        get() = FitMode.fromKey(prefs.getString(KEY_FIT_MODE, null))
        set(value) = prefs.edit().putString(KEY_FIT_MODE, value.key).apply()

    /**
     * The macOS port's "pause on battery" inverted.
     *
     * On a laptop, mains power is the normal state and battery is the exception
     * worth naming. On a phone it is the other way round — a setting called
     * "pause on battery" would mean "never render", which is not a setting, it
     * is an uninstall. So the opt-in is the useful half: render only while
     * something is charging it.
     */
    var renderOnlyWhileCharging: Boolean
        get() = prefs.getBoolean(KEY_ONLY_CHARGING, false)
        set(value) = prefs.edit().putBoolean(KEY_ONLY_CHARGING, value).apply()

    /** Written by the engine, read by the settings screen. */
    var status: String
        get() = prefs.getString(KEY_STATUS, "Not running") ?: "Not running"
        set(value) = prefs.edit().putString(KEY_STATUS, value).apply()

    fun observe(listener: SharedPreferences.OnSharedPreferenceChangeListener) {
        prefs.registerOnSharedPreferenceChangeListener(listener)
    }

    fun stopObserving(listener: SharedPreferences.OnSharedPreferenceChangeListener) {
        prefs.unregisterOnSharedPreferenceChangeListener(listener)
    }

    companion object {
        const val KEY_SELECTED = "selectedWallpaperId"
        const val KEY_PRESET = "importPresetName"
        const val KEY_FIT_MODE = "fitMode"
        const val KEY_ONLY_CHARGING = "renderOnlyWhileCharging"
        const val KEY_STATUS = "status"

        /** The keys that mean the engine has to do something. [KEY_STATUS] is
         *  the engine's own output and must not feed back into it. */
        val ENGINE_KEYS = setOf(KEY_SELECTED, KEY_FIT_MODE, KEY_ONLY_CHARGING)
    }
}
