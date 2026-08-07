// The fallback video path: an ordinary RGBA texture uploaded from system
// memory.
//
// Reached when the driver has no dmabuf import, or when the decode happened in
// software because there is no VA-API device. The conversion out of YUV has
// already happened on the CPU in swscale by the time a frame gets here, which
// is why this shader is trivial and why the README quotes a very different CPU
// figure for this path.
precision mediump float;

uniform sampler2D uTexture;

varying vec2 vTexCoord;

void main() {
    if (vTexCoord.x < 0.0 || vTexCoord.x > 1.0 || vTexCoord.y < 0.0 || vTexCoord.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    gl_FragColor = vec4(texture2D(uTexture, vTexCoord).rgb, 1.0);
}
