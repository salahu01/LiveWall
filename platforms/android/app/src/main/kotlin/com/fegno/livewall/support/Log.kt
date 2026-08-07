package com.fegno.livewall.support

import android.util.Log as AndroidLog

/**
 * Errors always go to logcat; the play-by-play only when it is asked for.
 *
 * The macOS port gates its verbose channel on a `LIVEWALL_VERBOSE` environment
 * variable, which a wallpaper service has no way to receive. The platform's own
 * equivalent is the tag level:
 *
 *     adb shell setprop log.tag.LiveWall VERBOSE
 *
 * `isLoggable` is a cheap property read, and gating at the call site means the
 * string interpolation never runs when it is off.
 */
object Log {

    const val TAG = "LiveWall"

    val verbose: Boolean
        get() = AndroidLog.isLoggable(TAG, AndroidLog.VERBOSE)

    inline fun info(message: () -> String) {
        if (verbose) AndroidLog.i(TAG, message())
    }

    fun info(message: String) {
        if (verbose) AndroidLog.i(TAG, message)
    }

    fun error(message: String) {
        AndroidLog.e(TAG, message)
    }
}
