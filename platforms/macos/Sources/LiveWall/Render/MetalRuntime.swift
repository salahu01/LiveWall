import Foundation
import Metal

/// One device, one queue, one pipeline — shared by every screen.
///
/// Two things here matter for footprint:
///
/// 1. **The shader is loaded from a precompiled metallib, not compiled from
///    source at launch.** `makeLibrary(source:)` brings the Metal runtime
///    compiler into the process, which measured at ~97 MB of resident graphics
///    memory that never comes back — an order of magnitude more than everything
///    else the app allocates put together. `tools/bundle.sh` compiles
///    `Resources/wallpaper.metal` ahead of time.
/// 2. **State is created once and reused.** Per-screen devices and pipelines
///    would multiply the fixed cost by the display count for no benefit.
///
/// The from-source path is kept only as a fallback for running the bare SwiftPM
/// binary without a bundle during development, and logs a warning when it fires.
final class MetalRuntime {

    static let shared = MetalRuntime()

    private(set) var device: MTLDevice?
    private(set) var queue: MTLCommandQueue?
    private var pipelines: [MTLPixelFormat: MTLRenderPipelineState] = [:]
    private var library: MTLLibrary?
    private var initialised = false

    private init() {}

    /// Returns a pipeline for the given colour format, building the shared
    /// device/queue/library on first use.
    func pipeline(for pixelFormat: MTLPixelFormat) -> MTLRenderPipelineState? {
        if let existing = pipelines[pixelFormat] { return existing }
        guard setUpIfNeeded(), let device, let library else { return nil }

        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "wallpaper_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "wallpaper_fragment")
        descriptor.colorAttachments[0].pixelFormat = pixelFormat

        do {
            let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
            pipelines[pixelFormat] = pipeline
            return pipeline
        } catch {
            Log.error("pipeline creation failed: \(error)")
            return nil
        }
    }

    private func setUpIfNeeded() -> Bool {
        if initialised { return device != nil && library != nil }
        initialised = true

        guard let device = MTLCreateSystemDefaultDevice() else {
            Log.error("no Metal device available")
            return false
        }
        self.device = device
        self.queue = device.makeCommandQueue()

        if let url = Bundle.main.url(forResource: "wallpaper", withExtension: "metallib"),
           let compiled = try? device.makeLibrary(URL: url) {
            library = compiled
            Log.info("loaded precompiled metallib")
        } else if let fallback = try? device.makeLibrary(source: Self.fallbackSource, options: nil) {
            // Costs ~97 MB of resident graphics memory. Development only.
            Log.error("wallpaper.metallib missing — compiling from source (high memory). Run tools/bundle.sh.")
            library = fallback
        } else {
            Log.error("could not create Metal library")
            return false
        }

        return library != nil && queue != nil
    }

    /// Kept in sync with `Resources/wallpaper.metal`.
    private static let fallbackSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct VOut {
        float4 position [[position]];
        float2 uv;
    };

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
    """
}
