package com.fegno.livewall.render

import android.opengl.EGLSurface
import android.opengl.GLES20
import android.view.Surface

/**
 * The wallpaper's `Surface`, wrapped in an EGL window surface and the two
 * programs that draw into it.
 *
 * Owned by the engine rather than by a source, because sources come and go — a
 * selection change swaps a video for a gradient — and rebuilding the EGL context
 * for each one would drop the last posted buffer and flash the wallpaper black
 * in between.
 *
 * All methods must be called on the render thread; the EGL context is bound
 * there and nowhere else.
 */
class RenderTarget(surface: Surface) {

    private val egl = EglCore(recordable = false)
    private var eglSurface: EGLSurface? = egl.createWindowSurface(surface)

    var width = 0
        private set
    var height = 0
        private set

    private var oes: OesQuadProgram? = null
    private var gradient: GradientProgram? = null

    fun resize(width: Int, height: Int) {
        this.width = width
        this.height = height
        makeCurrent()
        GLES20.glViewport(0, 0, width, height)
    }

    fun makeCurrent() {
        eglSurface?.let { egl.makeCurrent(it) }
    }

    fun swap(): Boolean = eglSurface?.let { egl.swapBuffers(it) } ?: false

    /** Programs are built on first use, which is always after [makeCurrent] —
     *  compiling a shader without a current context silently produces a program
     *  that never draws. */
    fun oesProgram(): OesQuadProgram = oes ?: OesQuadProgram().also { oes = it }

    fun gradientProgram(): GradientProgram = gradient ?: GradientProgram().also { gradient = it }

    fun clearToBlack() {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
    }

    fun release() {
        makeCurrent()
        oes?.release()
        gradient?.release()
        oes = null
        gradient = null
        egl.releaseSurface(eglSurface)
        eglSurface = null
        egl.release()
    }
}
