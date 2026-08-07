package com.fegno.livewall.support

import android.os.Debug

/**
 * Resident memory, for the log lines that trace activate/deactivate.
 *
 * PSS is the closest thing Android has to the `footprint(1)` number the macOS
 * port quotes: it divides shared pages by the number of processes sharing them,
 * which for a wallpaper service holding decoder buffers is the honest figure.
 * It is also expensive — reading it walks `/proc/self/smaps` — so this is only
 * ever called from a verbose-gated log line.
 */
object Footprint {

    fun formatted(): String {
        if (!Log.verbose) return ""
        val kb = Debug.getPss()
        return "%.1f MB".format(kb / 1024.0)
    }
}
