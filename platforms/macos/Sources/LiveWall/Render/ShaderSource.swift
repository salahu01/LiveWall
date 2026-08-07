import AppKit
import Metal
import QuartzCore

/// Draws a procedural wallpaper with a Metal fragment shader.
///
/// This is the floor of what the app can cost: no file, no decoder, no pixel
/// buffer pool. The only per-screen allocation is the drawable ring, held to two
/// buffers at a capped render resolution — a smooth gradient behind desktop
/// icons gains nothing from native-resolution drawables and would cost several
/// times as much to have them.
///
/// Device, queue and pipeline all come from `MetalRuntime` so the fixed Metal
/// cost is paid once for the whole app rather than once per display.
final class ShaderSource: NSObject, WallpaperSource {

    /// Longest edge of the render target. Anything above this is upscaled by
    /// the compositor for free.
    private static let maxRenderEdge: CGFloat = 1280

    private let metalLayer = CAMetalLayer()
    private var pipeline: MTLRenderPipelineState?

    private var screen: NSScreen?
    private var displayLink: CADisplayLink?
    private let targetFPS: Int

    private var frameIndex: UInt64 = 0

    private(set) var isActive = false

    var layer: CALayer { metalLayer }
    var summary: String { "Procedural (\(targetFPS) fps)" }

    init(fps: Int = 15) {
        self.targetFPS = max(1, fps)
        super.init()

        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        // Transparent until the first draw lands, so a shader that never gets to
        // run leaves the system wallpaper visible rather than a black rectangle.
        metalLayer.isOpaque = false
        // Default is 3. One fewer full-size buffer, and at wallpaper frame
        // rates there is no throughput to lose.
        metalLayer.maximumDrawableCount = 2
        metalLayer.allowsNextDrawableTimeout = true
        metalLayer.backgroundColor = NSColor.clear.cgColor
    }

    // MARK: - WallpaperSource

    func updateGeometry(size: CGSize, scale: CGFloat) {
        metalLayer.frame = CGRect(origin: .zero, size: size)
        metalLayer.contentsScale = scale

        // Render below native resolution and let the layer upscale.
        let longest = max(size.width, size.height)
        let factor = longest > Self.maxRenderEdge ? Self.maxRenderEdge / longest : 1.0
        let drawable = CGSize(width: floor(size.width * factor),
                              height: floor(size.height * factor))
        if drawable.width >= 1 && drawable.height >= 1 {
            metalLayer.drawableSize = drawable
        }
    }

    func attach(to screen: NSScreen) {
        self.screen = screen
        if isActive { startDisplayLink() }
    }

    func activate() {
        guard !isActive else { return }

        let runtime = MetalRuntime.shared
        guard let device = deviceForRuntime(runtime),
              let pipeline = runtime.pipeline(for: metalLayer.pixelFormat) else { return }

        metalLayer.device = device
        self.pipeline = pipeline

        isActive = true
        startDisplayLink()
        Log.info("shader activated — \(Footprint.formatted())")
    }

    func deactivate() {
        guard isActive else { return }
        isActive = false
        stopDisplayLink()
        // The last drawable stays presented; stopping the link is enough to
        // drop GPU and CPU cost to zero.
        Log.info("shader deactivated — \(Footprint.formatted())")
    }

    private func deviceForRuntime(_ runtime: MetalRuntime) -> MTLDevice? {
        // Touching `pipeline(for:)` is what lazily builds the device, so ask for
        // the pipeline first and then read the device back.
        _ = runtime.pipeline(for: metalLayer.pixelFormat)
        return runtime.device
    }

    // MARK: - Display link

    private func startDisplayLink() {
        stopDisplayLink()
        guard let screen else { return }
        let link = screen.displayLink(target: self, selector: #selector(tick))
        let fps = Float(targetFPS)
        link.preferredFrameRateRange = CAFrameRateRange(minimum: fps, maximum: fps, preferred: fps)
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
    }

    @objc private func tick() {
        guard isActive,
              let queue = MetalRuntime.shared.queue,
              let pipeline,
              let drawable = metalLayer.nextDrawable(),
              let buffer = queue.makeCommandBuffer()
        else { return }

        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = drawable.texture
        descriptor.colorAttachments[0].loadAction = .dontCare
        descriptor.colorAttachments[0].storeAction = .store

        guard let encoder = buffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        frameIndex += 1
        var time = Float(frameIndex) / Float(targetFPS)

        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentBytes(&time, length: MemoryLayout<Float>.size, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()

        buffer.present(drawable)
        buffer.commit()
    }
}
