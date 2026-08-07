package com.fegno.livewall.render

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES20
import com.fegno.livewall.support.Log

/**
 * The EGL display, config and context, and the window surfaces built from them.
 *
 * Both halves of the app need this: playback draws the decoder's external
 * texture onto the wallpaper's `Surface`, and import draws the same texture onto
 * the encoder's input `Surface`. It is the same six calls either way, so they
 * are written once.
 *
 * There is a deliberate asymmetry with the macOS port here. There, deactivating
 * releases the decoder *and* nothing else, because the layer is owned by the
 * window server and holding it costs the app nothing. The same reasoning applies
 * to this context: the decoder and its buffers are megabytes per frame, the EGL
 * context and one linked program are not, and destroying the EGL surface would
 * also drop the last posted buffer — turning "stopped wallpaper looks like a
 * still" into "stopped wallpaper is black". So teardown releases the codec and
 * keeps this.
 */
class EglCore(recordable: Boolean = false, preferTenBit: Boolean = false) {

    var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
        private set
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var config: EGLConfig? = null

    /** Whether the config actually handed back has 10 bits per channel. Asking
     *  for it is not the same as getting it, and the library records what the
     *  file really is rather than what was requested. */
    var isTenBit: Boolean = false
        private set

    init {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        check(display != EGL14.EGL_NO_DISPLAY) { "no EGL display" }

        val version = IntArray(2)
        check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize failed" }

        if (preferTenBit) {
            config = chooseConfig(recordable, tenBit = true)
            isTenBit = config != null
        }
        if (config == null) {
            config = chooseConfig(recordable, tenBit = false)
        }
        val chosen = config ?: error("no usable EGL config")

        // ES 3.0 first: the 10-bit path wants it, and every device this app's
        // minSdk admits has it. ES 2.0 is the floor the shaders are written to.
        context = createContext(chosen, 3) ?: createContext(chosen, 2)
            ?: error("could not create an EGL context")
    }

    private fun chooseConfig(recordable: Boolean, tenBit: Boolean): EGLConfig? {
        val attributes = ArrayList<Int>()
        attributes += listOf(
            EGL14.EGL_RED_SIZE, if (tenBit) 10 else 8,
            EGL14.EGL_GREEN_SIZE, if (tenBit) 10 else 8,
            EGL14.EGL_BLUE_SIZE, if (tenBit) 10 else 8,
            EGL14.EGL_ALPHA_SIZE, if (tenBit) 2 else 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT
        )
        // Without this the driver may hand back a config the video encoder
        // cannot consume, and the failure shows up as a green or empty file
        // rather than an error.
        if (recordable) attributes += listOf(EGL_RECORDABLE_ANDROID, 1)
        attributes += EGL14.EGL_NONE

        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        val ok = EGL14.eglChooseConfig(
            display, attributes.toIntArray(), 0, configs, 0, configs.size, count, 0
        )
        return if (ok && count[0] > 0) configs[0] else null
    }

    private fun createContext(config: EGLConfig, clientVersion: Int): EGLContext? {
        val attributes = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, clientVersion, EGL14.EGL_NONE)
        val candidate = EGL14.eglCreateContext(
            display, config, EGL14.EGL_NO_CONTEXT, attributes, 0
        )
        // eglCreateContext leaves an error latched on failure; clear it so the
        // fallback attempt isn't diagnosed against a stale one.
        EGL14.eglGetError()
        return candidate.takeIf { it != EGL14.EGL_NO_CONTEXT }
    }

    /** [surface] is an `android.view.Surface` or a `SurfaceTexture`. */
    fun createWindowSurface(surface: Any): EGLSurface {
        val attributes = intArrayOf(EGL14.EGL_NONE)
        val eglSurface = EGL14.eglCreateWindowSurface(display, config, surface, attributes, 0)
        check(eglSurface != null && eglSurface != EGL14.EGL_NO_SURFACE) {
            "eglCreateWindowSurface failed: 0x${Integer.toHexString(EGL14.eglGetError())}"
        }
        return eglSurface
    }

    fun makeCurrent(eglSurface: EGLSurface) {
        if (!EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context)) {
            Log.error("eglMakeCurrent failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
        }
    }

    fun makeNothingCurrent() {
        EGL14.eglMakeCurrent(
            display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT
        )
    }

    fun swapBuffers(eglSurface: EGLSurface): Boolean = EGL14.eglSwapBuffers(display, eglSurface)

    /**
     * Stamps the frame the encoder is about to consume. Without it the encoder
     * timestamps from wall-clock arrival, which turns a paced 24 fps grid back
     * into whatever cadence the transcode loop happened to run at.
     */
    fun setPresentationTime(eglSurface: EGLSurface, nanoseconds: Long) {
        EGLExt.eglPresentationTimeANDROID(display, eglSurface, nanoseconds)
    }

    fun releaseSurface(eglSurface: EGLSurface?) {
        if (eglSurface == null || eglSurface == EGL14.EGL_NO_SURFACE) return
        EGL14.eglDestroySurface(display, eglSurface)
    }

    fun release() {
        if (display == EGL14.EGL_NO_DISPLAY) return
        makeNothingCurrent()
        EGL14.eglDestroyContext(display, context)
        EGL14.eglReleaseThread()
        EGL14.eglTerminate(display)
        display = EGL14.EGL_NO_DISPLAY
        context = EGL14.EGL_NO_CONTEXT
        config = null
    }

    companion object {
        private const val EGL_RECORDABLE_ANDROID = 0x3142

        /** Fails loudly and immediately, because a GL error surfaces later as a
         *  black wallpaper with nothing in the log to explain it. */
        fun checkGl(operation: String) {
            val error = GLES20.glGetError()
            if (error != GLES20.GL_NO_ERROR) {
                error("$operation: GL error 0x${Integer.toHexString(error)}")
            }
        }
    }
}
