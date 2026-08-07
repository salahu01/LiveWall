package com.fegno.livewall.render

import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.Matrix
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * The two shader programs the app draws with, and the one quad they both use.
 *
 * Everything here is GLSL ES 1.00 against a two-triangle strip covering the
 * whole viewport in clip space. There is no vertex buffer object, no VAO and no
 * index buffer: four vertices from a client-side array is less state to get
 * wrong than the machinery to avoid re-uploading 32 bytes.
 */
private val QUAD: FloatBuffer = ByteBuffer
    .allocateDirect(8 * java.lang.Float.BYTES)
    .order(ByteOrder.nativeOrder())
    .asFloatBuffer()
    .apply {
        put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
        position(0)
    }

internal fun compileProgram(vertexSource: String, fragmentSource: String): Int {
    fun shader(type: Int, source: String): Int {
        val id = GLES20.glCreateShader(type)
        GLES20.glShaderSource(id, source)
        GLES20.glCompileShader(id)
        val status = IntArray(1)
        GLES20.glGetShaderiv(id, GLES20.GL_COMPILE_STATUS, status, 0)
        check(status[0] != 0) { "shader failed to compile: ${GLES20.glGetShaderInfoLog(id)}" }
        return id
    }

    val vertex = shader(GLES20.GL_VERTEX_SHADER, vertexSource)
    val fragment = shader(GLES20.GL_FRAGMENT_SHADER, fragmentSource)
    val program = GLES20.glCreateProgram()
    GLES20.glAttachShader(program, vertex)
    GLES20.glAttachShader(program, fragment)
    GLES20.glLinkProgram(program)
    val status = IntArray(1)
    GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
    check(status[0] != 0) { "program failed to link: ${GLES20.glGetProgramInfoLog(program)}" }
    // The program holds its own reference once linked.
    GLES20.glDeleteShader(vertex)
    GLES20.glDeleteShader(fragment)
    return program
}

/**
 * Draws the decoder's output — a `GL_TEXTURE_EXTERNAL_OES` texture fed by a
 * `SurfaceTexture` — onto whatever surface is current.
 *
 * Two transforms, kept separate because they answer different questions:
 *
 * - **`uScale`** is the fit mode, applied to the *geometry*. Fill scales the
 *   quad past the viewport and lets the rasteriser clip; fit scales it in and
 *   leaves the clear colour showing. One `glUniform2f` per frame, which is why
 *   switching modes never touches the decoder.
 * - **`uTexMatrix`** is the `SurfaceTexture` transform composed with the
 *   source's rotation metadata, applied to the *texture coordinates*. Rotation
 *   has to go here rather than on the geometry: the quad already covers a
 *   viewport whose aspect ratio is the output's, and rotating it would leave
 *   the corners empty.
 *
 * Never asking for a specific pixel format is the same discipline the macOS port
 * arrived at the hard way. An external texture takes the decoder's own output —
 * hardware HEVC hands back 4:2:0 at the file's bit depth — and the YUV→RGB
 * conversion happens in the sampler, on the GPU's fixed-function path. Copying
 * it through an `ImageReader` to name a format would cost a full-frame round
 * trip per frame for nothing.
 */
class OesQuadProgram {

    private val program = compileProgram(VERTEX, FRAGMENT)
    private val aPosition = GLES20.glGetAttribLocation(program, "aPosition")
    private val uTexMatrix = GLES20.glGetUniformLocation(program, "uTexMatrix")
    private val uScale = GLES20.glGetUniformLocation(program, "uScale")

    fun draw(textureId: Int, texMatrix: FloatArray, scaleX: Float, scaleY: Float) {
        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(uTexMatrix, 1, false, texMatrix, 0)
        GLES20.glUniform2f(uScale, scaleX, scaleY)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)

        GLES20.glEnableVertexAttribArray(aPosition)
        GLES20.glVertexAttribPointer(aPosition, 2, GLES20.GL_FLOAT, false, 0, QUAD)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(aPosition)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0)
    }

    fun release() {
        GLES20.glDeleteProgram(program)
    }

    companion object {
        private const val VERTEX = """
            attribute vec2 aPosition;
            uniform mat4 uTexMatrix;
            uniform vec2 uScale;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPosition * uScale, 0.0, 1.0);
                vec2 uv = aPosition * 0.5 + 0.5;
                vTexCoord = (uTexMatrix * vec4(uv, 0.0, 1.0)).xy;
            }
        """

        private const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord);
            }
        """

        /**
         * `SurfaceTexture` transform composed with a clockwise rotation of
         * [degrees] applied about the centre of the texture.
         *
         * Direction, worked out once so it doesn't have to be again: for a
         * portrait clip stored landscape with a 90° flag, the output's top-left
         * must sample the source's bottom-left. In GL texture space (Y up) that
         * is uv (0,1) → (0,0), which relative to the centre is (-0.5, 0.5) →
         * (-0.5, -0.5) — a *positive* (counter-clockwise) rotation of the
         * coordinates.
         */
        fun rotatedTexMatrix(surfaceTextureMatrix: FloatArray, degrees: Int): FloatArray {
            if (degrees % 360 == 0) return surfaceTextureMatrix

            val rotation = FloatArray(16)
            Matrix.setIdentityM(rotation, 0)
            Matrix.translateM(rotation, 0, 0.5f, 0.5f, 0f)
            Matrix.rotateM(rotation, 0, degrees.toFloat(), 0f, 0f, 1f)
            Matrix.translateM(rotation, 0, -0.5f, -0.5f, 0f)

            val combined = FloatArray(16)
            Matrix.multiplyMM(combined, 0, surfaceTextureMatrix, 0, rotation, 0)
            return combined
        }

        /** A texture name configured the way an external texture has to be:
         *  no mips, clamped, linear. */
        fun createExternalTexture(): Int {
            val names = IntArray(1)
            GLES20.glGenTextures(1, names, 0)
            val id = names[0]
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, id)
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE
            )
            GLES20.glTexParameteri(
                GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE
            )
            EglCore.checkGl("createExternalTexture")
            return id
        }
    }
}

/**
 * The procedural mode: three colour stops interpolated across the diagonal,
 * evaluated per pixel on the GPU.
 *
 * The macOS port's equivalent costs the app literally zero CPU, because it hands
 * `CAGradientLayer` keyframes to the render server once and the compositor
 * interpolates them from then on. Android has no such thing — SurfaceFlinger
 * composites the buffer you posted and will not animate it for you — so the
 * drift has to be driven from this process. What that costs is one `glClear`,
 * four vertices and an `eglSwapBuffers` per drawn frame, at the ten frames a
 * second in [GradientSource]. It is not free, and the README says so.
 */
class GradientProgram {

    private val program = compileProgram(VERTEX, FRAGMENT)
    private val aPosition = GLES20.glGetAttribLocation(program, "aPosition")
    private val uColor0 = GLES20.glGetUniformLocation(program, "uColor0")
    private val uColor1 = GLES20.glGetUniformLocation(program, "uColor1")
    private val uColor2 = GLES20.glGetUniformLocation(program, "uColor2")
    private val uMid = GLES20.glGetUniformLocation(program, "uMid")

    fun draw(color0: FloatArray, color1: FloatArray, color2: FloatArray, mid: Float) {
        GLES20.glUseProgram(program)
        GLES20.glUniform3f(uColor0, color0[0], color0[1], color0[2])
        GLES20.glUniform3f(uColor1, color1[0], color1[1], color1[2])
        GLES20.glUniform3f(uColor2, color2[0], color2[1], color2[2])
        GLES20.glUniform1f(uMid, mid)

        GLES20.glEnableVertexAttribArray(aPosition)
        GLES20.glVertexAttribPointer(aPosition, 2, GLES20.GL_FLOAT, false, 0, QUAD)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        GLES20.glDisableVertexAttribArray(aPosition)
    }

    fun release() {
        GLES20.glDeleteProgram(program)
    }

    companion object {
        private const val VERTEX = """
            attribute vec2 aPosition;
            varying vec2 vUv;
            void main() {
                vUv = aPosition * 0.5 + 0.5;
                gl_Position = vec4(aPosition, 0.0, 1.0);
            }
        """

        // Written with mix/step rather than a branch: a ternary on a vec3 is not
        // portable across GLSL ES 1.00 compilers, and select-both is one extra
        // mix on a shader that runs at 10 fps.
        private const val FRAGMENT = """
            precision mediump float;
            varying vec2 vUv;
            uniform vec3 uColor0;
            uniform vec3 uColor1;
            uniform vec3 uColor2;
            uniform float uMid;
            void main() {
                float t = clamp((vUv.x + vUv.y) * 0.5, 0.0, 1.0);
                vec3 low = mix(uColor0, uColor1, clamp(t / max(uMid, 0.0001), 0.0, 1.0));
                vec3 high = mix(uColor1, uColor2, clamp((t - uMid) / max(1.0 - uMid, 0.0001), 0.0, 1.0));
                gl_FragColor = vec4(mix(low, high, step(uMid, t)), 1.0);
            }
        """
    }
}
