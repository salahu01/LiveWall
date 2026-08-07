package com.fegno.livewall.support

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.PowerManager

/**
 * Watches every system condition that should stop wallpaper rendering.
 *
 * This is the machine-wide half. The other half — "can anyone actually see the
 * wallpaper right now" — is one line on Android, and it is worth being explicit
 * about how much that removes. The macOS port derives an uncovered-desktop
 * fraction from the window list because `NSWindow.occlusionState` is a yes/no
 * and a window covering all but a corner still reports the desktop as visible.
 * Android's `WallpaperService.Engine.onVisibilityChanged` is the system telling
 * you directly, and it already folds in the screen being off, the device being
 * locked, and any app being in the foreground. So screen lock, display sleep,
 * screensaver and the fifteen-minute idle timer all have no counterpart here:
 * every one of them arrives as `onVisibilityChanged(false)`.
 *
 * What remains is what the system will not tell you: thermal pressure, battery
 * saver, a nearly flat battery, and the user's own opt-in.
 *
 * Every one of these is a hard stop rather than a slowdown, and that is forced
 * by the decode path: playback pulls one frame per tick and the assets have no
 * B-frames, so every frame is a reference frame that must be decoded whether or
 * not it is shown. Lowering the tick rate would play the clip in slow motion,
 * not more cheaply. Stopping is both free and graceful — the last frame stays
 * posted on the surface, so a stopped wallpaper looks like a still rather than a
 * black rectangle.
 */
class PowerGate(context: Context) {

    private val appContext = context.applicationContext
    private val power = appContext.getSystemService(Context.POWER_SERVICE) as PowerManager

    /** Called whenever the answer to "should we render at all?" may have changed. */
    var onChange: (() -> Unit)? = null

    /** User setting: render only while the device is charging. */
    var renderOnlyWhileCharging: Boolean = false
        set(value) {
            if (field == value) return
            field = value
            onChange?.invoke()
        }

    private var thermalStatus: Int = PowerManager.THERMAL_STATUS_NONE
    private var charging: Boolean = true
    private var batteryFraction: Double? = null
    private var started = false

    /**
     * `THERMAL_STATUS_SEVERE` is the point at which the platform is already
     * throttling and telling foreground apps to shed work. A wallpaper has no
     * business competing there. `MODERATE` is deliberately not included — on a
     * phone it is reached by ordinary things like charging in a warm room, and
     * stopping the wallpaper every time the device warms up reads as a bug.
     */
    private val thermallyPressured: Boolean
        get() = thermalStatus >= PowerManager.THERMAL_STATUS_SEVERE

    /**
     * Below this, off charge, the wallpaper stops regardless of the setting.
     * Ambient decoration is not what the last of a charge is for.
     */
    private val lowBatteryFraction = 0.20

    private val thermalListener = PowerManager.OnThermalStatusChangedListener { status ->
        if (status == thermalStatus) return@OnThermalStatusChangedListener
        thermalStatus = status
        Log.info { "thermal status now $status" }
        onChange?.invoke()
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                Intent.ACTION_BATTERY_CHANGED -> {
                    val wasCharging = charging
                    val previousFraction = batteryFraction
                    readBattery(intent)
                    // ACTION_BATTERY_CHANGED fires on every percent and every
                    // temperature tick. Only the two things the gate reads are
                    // worth waking the engine for.
                    val crossedLowMark = (previousFraction ?: 1.0) >= lowBatteryFraction !=
                        (batteryFraction ?: 1.0) >= lowBatteryFraction
                    if (wasCharging != charging || crossedLowMark) onChange?.invoke()
                }

                PowerManager.ACTION_POWER_SAVE_MODE_CHANGED -> onChange?.invoke()
            }
        }
    }

    fun start() {
        if (started) return
        started = true

        power.addThermalStatusListener(thermalListener)
        thermalStatus = power.currentThermalStatus

        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_BATTERY_CHANGED)
            addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
        }
        // ACTION_BATTERY_CHANGED is sticky, so this returns the current state
        // immediately rather than leaving the gate guessing until the next tick.
        val sticky = appContext.registerReceiver(receiver, filter)
        readBattery(sticky)
    }

    fun stop() {
        if (!started) return
        started = false
        runCatching { power.removeThermalStatusListener(thermalListener) }
        runCatching { appContext.unregisterReceiver(receiver) }
    }

    private fun readBattery(intent: Intent?) {
        if (intent == null) return
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL ||
            intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0

        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        batteryFraction = if (level >= 0 && scale > 0) level.toDouble() / scale else null
    }

    /**
     * Human-readable reason rendering is suspended, for the settings screen.
     * This is also the definition of [blocksRendering] — one list, so the UI can
     * never claim a reason the gate doesn't actually apply.
     */
    fun blockReason(): String? {
        if (thermallyPressured) return "device running hot"
        if (power.isPowerSaveMode) return "Battery Saver"
        if (!charging) {
            if (renderOnlyWhileCharging) return "not charging"
            batteryFraction?.let { if (it < lowBatteryFraction) return "battery low" }
        }
        return null
    }

    val blocksRendering: Boolean get() = blockReason() != null
}
