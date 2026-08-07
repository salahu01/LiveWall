// One full-screen triangle, shared by every fragment shader here.
//
// A triangle rather than a quad: two triangles meeting on the diagonal make the
// GPU shade the pixels along that seam twice, and the third vertex costs
// nothing. The positions are generated from gl_VertexID-free attribute data so
// this still compiles as GLES 2, where gl_VertexID does not exist.
attribute vec2 aPosition;

// scale.xy, offset.xy — the FitTransform. Applied here rather than in the
// fragment shader so it is three multiplies per frame instead of three per
// pixel.
uniform vec4 uFit;

varying vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);

    // Clip space is -1..1 with the origin at the bottom left; textures — and
    // every video frame that reaches them — run top-down. The flip belongs
    // here, once, rather than in every sampler.
    vec2 uv = aPosition * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;

    vTexCoord = uv * uFit.xy + uFit.zw;
}
