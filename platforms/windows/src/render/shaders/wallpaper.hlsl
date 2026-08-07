// Everything the app draws. Four entry points, all compiled to byte code at
// build time by cmake/CompileShaders.cmake — see the note there about why this
// is not compiled at runtime.
//
// There is no vertex buffer and no index buffer. The vertex shader synthesises
// a full-screen triangle from SV_VertexID, which is both cheaper than a quad
// (one triangle, no diagonal seam where the two halves of a quad meet) and
// removes an input layout, a buffer and a bind call from every frame.

cbuffer FrameConstants : register(b0)
{
    // Fit mode, as a scale on the quad. See FitMode.cpp.
    float2 g_fitScale;
    // Seconds since the gradient started, for the procedural mode. Unused by
    // the video paths.
    float  g_time;
    // 0 for limited-range (video) YUV, 1 for full range. Every file the
    // transcoder writes is limited range; this exists so a future import path
    // that preserves full-range content has somewhere to say so.
    float  g_fullRange;
};

Texture2D<float>  g_luma    : register(t0);   // Y plane  (R8 or R16)
Texture2D<float2> g_chroma  : register(t1);   // UV plane (R8G8 or R16G16)
SamplerState      g_sampler : register(s0);

struct Varyings
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// Vertex
// ---------------------------------------------------------------------------

Varyings VS_Fullscreen(uint id : SV_VertexID)
{
    // The standard three-vertex cover: (-1,-1), (3,-1), (-1,3) in clip space,
    // with UVs that land 0..1 across the visible square.
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 clip = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);

    Varyings output;
    // Fill scales the quad past the viewport, so the overflow is clipped by the
    // rasteriser — no sampling outside the texture, no border colour, nothing
    // to configure on the sampler. Fit scales it inwards and the untouched part
    // of the render target keeps whatever was there, which for a
    // DirectComposition surface with premultiplied alpha is transparent: the
    // bars show the user's real desktop wallpaper rather than black, exactly as
    // the macOS version's letterboxing does.
    output.position = float4(clip * g_fitScale, 0.0, 1.0);
    output.uv = uv;
    return output;
}

// ---------------------------------------------------------------------------
// YUV -> RGB
//
// The decoder hands back NV12 (8-bit) or P010 (10-bit) and the conversion
// happens here rather than being asked of Media Foundation. Enabling the video
// processor MFT instead would insert a whole extra transform in the decode
// path, allocate its own pool of output frames, and — on the machines where it
// falls back to software — cost more CPU than the decode itself. This is the
// Windows shape of the macOS finding that naming a pixel format made
// AVFoundation revalidate every buffer through a kernel round trip: ask the
// decoder for nothing it does not already produce, and do the cheap arithmetic
// yourself.
//
// BT.709, which is what the transcoder writes for everything above SD.
// ---------------------------------------------------------------------------

float3 yuvToRgb(float y, float2 uv, float fullRange)
{
    // Limited range: Y is 16..235 and chroma 16..240 over 8 bits. The two
    // scale factors below are 255/219 and 255/224.
    float yScale = lerp(1.16438356, 1.0, fullRange);
    float yOffset = lerp(0.06274510, 0.0, fullRange);
    float cScale = lerp(1.13868613, 1.0, fullRange);

    float  Y = (y - yOffset) * yScale;
    float2 C = (uv - 0.5) * cScale;

    return float3(
        Y + 1.79274107 * C.y,
        Y - 0.21324861 * C.x - 0.53290933 * C.y,
        Y + 2.11240179 * C.x);
}

float4 sampleVideo(float2 uv, float fullRange)
{
    float  y = g_luma.Sample(g_sampler, uv);
    float2 c = g_chroma.Sample(g_sampler, uv);
    float3 rgb = saturate(yuvToRgb(y, c, fullRange));
    // Premultiplied alpha, because the swap chain is composed by
    // DirectComposition over the system wallpaper. Alpha is 1 across the whole
    // frame — the transparency that matters is in the region the quad does not
    // cover at all.
    return float4(rgb, 1.0);
}

float4 PS_NV12(Varyings input) : SV_TARGET
{
    // R8 / R8G8 views over the two planes: the hardware normalises to 0..1 and
    // the arithmetic above is identical for both bit depths.
    return sampleVideo(input.uv, g_fullRange);
}

float4 PS_P010(Varyings input) : SV_TARGET
{
    // P010 stores 10 bits left-aligned in 16, so an R16_UNORM view reads
    // values scaled by 64/65535 rather than 1/1023 — a 0.1% offset that is
    // below the quantisation of the output and not worth a multiply per pixel
    // to correct. The separate entry point exists because the two formats need
    // different shader resource views, not different maths.
    return sampleVideo(input.uv, g_fullRange);
}

// ---------------------------------------------------------------------------
// Procedural
//
// The cheapest wallpaper the app can draw. Three crossed sinusoids at 15 fps
// over a full-screen triangle: no decode, no texture upload, no video memory
// beyond the render target the swap chain already owns.
//
// The macOS app's equivalent is a CAGradientLayer the compositor interpolates,
// which costs the app literally zero because the render server owns the
// animation. Windows has no equivalent — DirectComposition can animate a
// transform or an opacity, but not the colours of a gradient — so this pays a
// real, small cost instead: one triangle of ALU work, fifteen times a second.
// ---------------------------------------------------------------------------

float4 PS_Gradient(Varyings input) : SV_TARGET
{
    float2 p = input.uv;
    float t = g_time;

    // Periods chosen coprime-ish so the pair does not visibly repeat, matching
    // the 24 s / 37 s pairing the macOS gradient uses.
    float a = 0.5 + 0.5 * sin(6.0 * p.x + 6.2831853 * t / 24.0);
    float b = 0.5 + 0.5 * cos(7.0 * (1.0 - p.y) - 6.2831853 * t / 37.0);
    float c = 0.5 + 0.5 * sin(5.0 * (0.7 * p.x + 0.7 * (1.0 - p.y)) + 6.2831853 * t / 19.0);

    float field = (a + b + c) / 3.0;
    float glow = pow(field, 3.0);

    // The same deep blue-violet palette as the macOS gradient, so the two
    // platforms' procedural modes are recognisably the same wallpaper.
    float3 base    = float3(0.04, 0.05, 0.11);
    float3 sweep   = float3(0.16, 0.10, 0.30);
    float3 accent  = float3(0.03, 0.19, 0.24);

    float3 colour = lerp(base, sweep, field) + accent * glow;

    // Slight corner falloff so the frame has a centre.
    float2 d = p - 0.5;
    colour *= 1.0 - 0.35 * dot(d, d);

    return float4(saturate(colour), 1.0);
}
