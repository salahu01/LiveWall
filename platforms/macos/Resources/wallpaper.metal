#include <metal_stdlib>
using namespace metal;

struct VOut {
    float4 position [[position]];
    float2 uv;
};

// Single oversized triangle — no vertex buffer to allocate.
vertex VOut wallpaper_vertex(uint vid [[vertex_id]]) {
    float2 corners[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VOut out;
    out.position = float4(corners[vid], 0.0, 1.0);
    out.uv = corners[vid] * 0.5 + 0.5;
    return out;
}

fragment float4 wallpaper_fragment(VOut in [[stage_in]],
                                   constant float &time [[buffer(0)]]) {
    float2 uv = in.uv;

    // Two slow sinusoids crossed into a drifting field. Deliberately cheap:
    // a handful of ALU ops, no loops, no texture fetches.
    float a = sin(uv.x * 3.0 + time * 0.20);
    float b = cos(uv.y * 3.0 - time * 0.15);
    float field = 0.5 + 0.5 * (a * b);

    float3 deep   = float3(0.04, 0.05, 0.11);
    float3 violet = float3(0.16, 0.10, 0.30);
    float3 teal   = float3(0.03, 0.19, 0.24);

    float3 color = mix(deep, violet, uv.y);
    color = mix(color, teal, field * 0.7);

    float2 centered = uv - 0.5;
    color *= 1.0 - 0.45 * dot(centered, centered);

    return float4(color, 1.0);
}
