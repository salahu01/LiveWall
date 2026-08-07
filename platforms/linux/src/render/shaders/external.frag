// The zero-copy video path.
//
// The texture is a GL_TEXTURE_EXTERNAL_OES bound to an EGLImage that wraps the
// dmabuf VA-API decoded into. The driver does the YUV-to-RGB conversion in
// fixed-function hardware on the sampler, in the frame's own colour space and
// bit depth — which is why there is no colour matrix in this file and no
// separate 10-bit variant. Writing one by hand would mean picking a matrix,
// and the decoder already knows which one the file asked for.
#extension GL_OES_EGL_image_external : require
precision mediump float;

uniform samplerExternalOES uTexture;

varying vec2 vTexCoord;

void main() {
    // Outside the frame is the Fit mode's letterbox. Transparent rather than
    // black, so the bars show whatever the compositor has behind this surface —
    // on X11 with a compositor, that is the user's real desktop wallpaper.
    if (vTexCoord.x < 0.0 || vTexCoord.x > 1.0 || vTexCoord.y < 0.0 || vTexCoord.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    gl_FragColor = vec4(texture2D(uTexture, vTexCoord).rgb, 1.0);
}
