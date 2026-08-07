// The procedural mode.
//
// This is where the Linux port genuinely cannot match the macOS one, and it is
// worth being exact about why rather than pretending the difference away.
//
// macOS draws its procedural wallpaper with a CAGradientLayer holding
// keyframes. The app hands those to the render server once and then does
// nothing at all — the compositor interpolates them on the GPU on its own
// schedule, and the app's own CPU cost is literally zero. Neither X11 nor
// Wayland has an equivalent: there is no way to hand a compositor an animation
// and stop participating. Something has to submit each frame.
//
// So the cost here is one wakeup and one full-screen triangle per tick, at the
// rate `proceduralFps` sets — 10 by default. The shading is deliberately cheap:
// four smoothstep-blended stops along a rotating axis, no noise, no loops, no
// texture reads. On any GPU made this century the fragment cost is
// unmeasurable and what remains is the wakeup, which is why the setting exists
// and why the README quotes it.
precision mediump float;

uniform float uTime;
uniform vec2 uResolution;

varying vec2 vTexCoord;

// Four stops of a slow aurora. Chosen to stay dark enough to read a desktop
// icon against, which a saturated gradient is not.
const vec3 kStop0 = vec3(0.043, 0.055, 0.129);
const vec3 kStop1 = vec3(0.086, 0.196, 0.278);
const vec3 kStop2 = vec3(0.157, 0.361, 0.361);
const vec3 kStop3 = vec3(0.290, 0.196, 0.353);

void main() {
    // The gradient axis rotates through a full turn every ~6 minutes. Slow
    // enough that no single frame differs visibly from the one before it, which
    // is the property that lets the frame rate be this low without looking like
    // a slideshow.
    float angle = uTime * 0.0175;
    vec2 axis = vec2(cos(angle), sin(angle));

    // Centre the coordinates and correct for the output's aspect ratio, or the
    // bands run at a different angle on a 32:9 monitor than on a 16:9 one.
    vec2 centred = (vTexCoord - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float t = dot(centred, axis) * 0.7071 + 0.5;

    // A second, faster term keeps the bands from being perfectly straight.
    // Amplitude is small enough to read as a drift rather than a wave.
    t += 0.06 * sin(vTexCoord.y * 3.1416 + uTime * 0.11);
    t = clamp(t, 0.0, 1.0);

    vec3 colour = mix(kStop0, kStop1, smoothstep(0.00, 0.40, t));
    colour = mix(colour, kStop2, smoothstep(0.35, 0.72, t));
    colour = mix(colour, kStop3, smoothstep(0.68, 1.00, t));

    gl_FragColor = vec4(colour, 1.0);
}
