package com.fegno.livewall.app

import android.content.SharedPreferences
import android.os.Handler
import android.os.HandlerThread
import android.service.wallpaper.WallpaperService
import android.view.SurfaceHolder
import com.fegno.livewall.importer.Library
import com.fegno.livewall.render.GradientSource
import com.fegno.livewall.render.RenderTarget
import com.fegno.livewall.render.VideoSource
import com.fegno.livewall.render.WallpaperSource
import com.fegno.livewall.support.Log
import com.fegno.livewall.support.PowerGate
import com.fegno.livewall.support.Settings
import java.util.concurrent.CountDownLatch

/**
 * The wallpaper itself.
 *
 * This file is where the port is smallest, and that is the whole story of moving
 * this design to Android. On macOS the equivalent — `WallpaperEngine`,
 * `ScreenController`, `DesktopWindow` and `DesktopVisibility`, around 400 lines
 * between them — exists to put a window at `kCGDesktopWindowLevel`, keep one per
 * `NSScreen`, and work out from the window list what fraction of the desktop is
 * actually uncovered, because `NSWindow.occlusionState` is a yes/no and a window
 * covering all but a corner still reports the desktop as visible.
 *
 * Android hands all of that over in [Engine.onVisibilityChanged]. The system
 * knows whether the launcher is in front, whether the screen is on, and whether
 * the device is locked, and it tells the wallpaper directly. There is one
 * surface, sized by the system. The uncovered-fraction arithmetic, the
 * hysteresis thresholds, the window-level constants and the per-display
 * bookkeeping all simply have no counterpart.
 *
 * What survives intact is the part that matters: **teardown, not pause.**
 */
class LiveWallService : WallpaperService() {

    override fun onCreateEngine(): Engine = LiveWallEngine()

    private inner class LiveWallEngine : Engine() {

        private val settings = Settings(this@LiveWallService)
        private val library = Library(this@LiveWallService)
        private val power = PowerGate(this@LiveWallService)

        private lateinit var renderThread: HandlerThread
        private lateinit var renderHandler: Handler

        /**
         * `SurfaceTexture.onFrameAvailable` has to be delivered somewhere other
         * than the thread that waits for it, or the wait deadlocks the delivery.
         * The thread does nothing else — it sets a flag and notifies.
         */
        private lateinit var frameThread: HandlerThread
        private lateinit var frameHandler: Handler

        private var target: RenderTarget? = null
        private var source: WallpaperSource? = null

        private var visible = false
        private var surfaceReady = false

        private val settingsListener =
            SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
                if (key !in Settings.ENGINE_KEYS) return@OnSharedPreferenceChangeListener
                renderHandler.post {
                    when (key) {
                        Settings.KEY_SELECTED -> applySelection()
                        Settings.KEY_FIT_MODE -> source?.setFitMode(settings.fitMode)
                        Settings.KEY_ONLY_CHARGING ->
                            power.renderOnlyWhileCharging = settings.renderOnlyWhileCharging
                    }
                    publishStatus()
                }
            }

        /**
         * Visibility flaps during app-switch and recents animations. Tearing the
         * decoder down and rebuilding it on each flap costs more than the few
         * frames it saves, so teardown waits; re-activation never does.
         */
        private val teardown = Runnable {
            source?.deactivate()
            publishStatus()
        }

        // MARK: - Lifecycle

        override fun onCreate(surfaceHolder: SurfaceHolder) {
            super.onCreate(surfaceHolder)

            // A wallpaper that takes touches steals them from the launcher.
            setTouchEventsEnabled(false)

            renderThread = HandlerThread("livewall-render").apply { start() }
            renderHandler = Handler(renderThread.looper)
            frameThread = HandlerThread("livewall-frames").apply { start() }
            frameHandler = Handler(frameThread.looper)

            power.renderOnlyWhileCharging = settings.renderOnlyWhileCharging
            power.onChange = { renderHandler.post { evaluate() } }
            power.start()

            settings.observe(settingsListener)
        }

        override fun onSurfaceCreated(holder: SurfaceHolder) {
            super.onSurfaceCreated(holder)
            val surface = holder.surface
            renderHandler.post {
                runCatching { RenderTarget(surface) }
                    .onSuccess {
                        target = it
                        surfaceReady = true
                        applySelection()
                    }
                    .onFailure { Log.error("could not create the render target: $it") }
            }
        }

        override fun onSurfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            super.onSurfaceChanged(holder, format, width, height)
            renderHandler.post {
                target?.resize(width, height)
                source?.onSurfaceSizeChanged(width, height)
                evaluate()
            }
        }

        override fun onSurfaceDestroyed(holder: SurfaceHolder) {
            // Blocking on purpose: the surface is gone the moment this returns,
            // and GL calls against a dead surface are undefined rather than
            // merely useless.
            runOnRenderThreadBlocking {
                surfaceReady = false
                source?.release()
                source = null
                target?.release()
                target = null
            }
            super.onSurfaceDestroyed(holder)
        }

        override fun onVisibilityChanged(visible: Boolean) {
            super.onVisibilityChanged(visible)
            renderHandler.post {
                this.visible = visible
                Log.info { "visibility -> $visible" }
                evaluate()
            }
        }

        /**
         * Parallax as the launcher pages scroll is deliberately not implemented.
         * It would mean redrawing on every scroll frame — at the panel's refresh
         * rate, not the wallpaper's — which is the single most expensive thing
         * this design could agree to do, in exchange for an effect most users
         * never notice.
         */
        override fun onOffsetsChanged(
            xOffset: Float, yOffset: Float,
            xStep: Float, yStep: Float,
            xPixels: Int, yPixels: Int
        ) = Unit

        override fun onDestroy() {
            settings.stopObserving(settingsListener)
            power.onChange = null
            power.stop()

            runOnRenderThreadBlocking {
                renderHandler.removeCallbacks(teardown)
                source?.release()
                source = null
                target?.release()
                target = null
            }

            renderThread.quitSafely()
            frameThread.quitSafely()
            super.onDestroy()
        }

        // MARK: - Selection

        /** Render thread only. */
        private fun applySelection() {
            val target = this.target ?: return

            source?.release()
            source = null

            val item = library.item(settings.selectedId)
            if (item != null) {
                val file = library.file(item)
                if (file.isFile) {
                    val video = VideoSource(
                        file = file,
                        indexFps = item.fps,
                        bitDepth = item.pixelBitDepth,
                        fitMode = settings.fitMode,
                        target = target,
                        frameHandler = frameHandler
                    )
                    if (video.prepare()) {
                        source = video
                    } else {
                        Log.error("could not prepare ${item.title}; falling back to the gradient")
                    }
                } else {
                    Log.error("selected wallpaper missing on disk, falling back to the gradient")
                }
            }

            if (source == null) source = GradientSource(target)
            source?.onSurfaceSizeChanged(target.width, target.height)

            evaluate()
        }

        // MARK: - The gate

        /** Render thread only. */
        private fun evaluate() {
            if (!surfaceReady) return
            val shouldRender = visible && !power.blocksRendering

            if (shouldRender) {
                renderHandler.removeCallbacks(teardown)
                source?.activate()
            } else if (source?.isActive == true) {
                renderHandler.removeCallbacks(teardown)
                renderHandler.postDelayed(teardown, TEARDOWN_DELAY_MS)
            }

            publishStatus()
        }

        /** One line for the settings screen. Built from the same predicates the
         *  gate uses, so it can never claim a reason that isn't applied. */
        private fun publishStatus() {
            val status = when {
                power.blockReason() != null -> "Paused — ${power.blockReason()}"
                !visible -> "Paused — nothing is looking at it"
                source?.isActive == true -> "Rendering — ${source?.summary}"
                source == null -> "No wallpaper"
                else -> "Idle"
            }
            if (settings.status != status) settings.status = status
        }

        private fun runOnRenderThreadBlocking(block: () -> Unit) {
            if (!::renderHandler.isInitialized) return
            if (Thread.currentThread() === renderThread) {
                block()
                return
            }
            val latch = CountDownLatch(1)
            val posted = renderHandler.post {
                try {
                    block()
                } finally {
                    latch.countDown()
                }
            }
            if (!posted) return
            runCatching { latch.await() }
        }
    }

    private companion object {
        const val TEARDOWN_DELAY_MS = 400L
    }
}
