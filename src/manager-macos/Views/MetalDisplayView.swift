import SwiftUI
import MetalKit

// Metal-based display view for rendering VM framebuffer output.
// Receives BGRA pixel data from the VirtIO GPU device via the IPC bridge.

class MetalDisplayRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState

    private var texture: MTLTexture?
    private var textureWidth: Int = 0
    private var textureHeight: Int = 0
    private let textureLock = NSLock()
    
    weak var view: MTKView?

    private init(device: MTLDevice, queue: MTLCommandQueue, pipeline: MTLRenderPipelineState) {
        self.device = device
        self.commandQueue = queue
        self.pipelineState = pipeline
        super.init()
    }

    static func create() -> MetalDisplayRenderer? {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }

        guard let library = Self.loadShaderLibrary(device: device) else {
            print("[ERROR] MetalDisplayRenderer: failed to load shader library")
            return nil
        }

        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = library.makeFunction(name: "vertexShader")
        pipelineDescriptor.fragmentFunction = library.makeFunction(name: "fragmentShader")
        pipelineDescriptor.colorAttachments[0].pixelFormat = .bgra8Unorm

        guard let pipeline = try? device.makeRenderPipelineState(descriptor: pipelineDescriptor) else {
            return nil
        }

        return MetalDisplayRenderer(device: device, queue: queue, pipeline: pipeline)
    }

    private static func loadShaderLibrary(device: MTLDevice) -> MTLLibrary? {
        // Try default library first (works when built with Xcode / .metallib bundled)
        if let lib = device.makeDefaultLibrary() {
            return lib
        }

        // For SwiftPM builds: compile from source at runtime
        let shaderSource = """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float2 texCoord;
        };

        vertex VertexOut vertexShader(uint vertexId [[vertex_id]]) {
            constexpr float2 positions[4] = {
                float2(-1.0, -1.0),
                float2( 1.0, -1.0),
                float2(-1.0,  1.0),
                float2( 1.0,  1.0),
            };
            constexpr float2 texCoords[4] = {
                float2(0.0, 1.0),
                float2(1.0, 1.0),
                float2(0.0, 0.0),
                float2(1.0, 0.0),
            };

            VertexOut out;
            out.position = float4(positions[vertexId], 0.0, 1.0);
            out.texCoord = texCoords[vertexId];
            return out;
        }

        fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                       texture2d<float> tex [[texture(0)]]) {
            constexpr sampler s(mag_filter::linear, min_filter::linear);
            float4 color = tex.sample(s, in.texCoord);
            color.a = 1.0;
            return color;
        }
        """

        let options = MTLCompileOptions()
        return try? device.makeLibrary(source: shaderSource, options: options)
    }

    func blitDirtyRect(
        pixels: UnsafeRawPointer,
        dirtyX: Int, dirtyY: Int,
        dirtyWidth: Int, dirtyHeight: Int,
        srcStride: Int,
        resourceWidth: Int, resourceHeight: Int
    ) {
        textureLock.lock()
        defer { textureLock.unlock() }

        if texture == nil || textureWidth != resourceWidth || textureHeight != resourceHeight {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm,
                width: resourceWidth,
                height: resourceHeight,
                mipmapped: false
            )
            desc.usage = [.shaderRead]
            texture = device.makeTexture(descriptor: desc)
            textureWidth = resourceWidth
            textureHeight = resourceHeight
        }

        let clampedW = min(dirtyWidth, resourceWidth - dirtyX)
        let clampedH = min(dirtyHeight, resourceHeight - dirtyY)
        guard clampedW > 0 && clampedH > 0 else { return }

        texture?.replace(
            region: MTLRegionMake2D(dirtyX, dirtyY, clampedW, clampedH),
            mipmapLevel: 0,
            withBytes: pixels,
            bytesPerRow: srcStride
        )
        
        DispatchQueue.main.async { [weak self] in
            self?.view?.needsDisplay = true
        }
    }

    // Drop the cached framebuffer texture so the next draw shows the
    // MTKView's clear color (black) instead of the last captured frame.
    // Used when the guest OS blanks its display.
    func clear() {
        textureLock.lock()
        texture = nil
        textureWidth = 0
        textureHeight = 0
        textureLock.unlock()

        DispatchQueue.main.async { [weak self] in
            self?.view?.needsDisplay = true
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        textureLock.lock()
        let currentTexture = texture
        textureLock.unlock()

        guard let drawable = view.currentDrawable,
              let descriptor = view.currentRenderPassDescriptor else { return }

        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        if let tex = currentTexture {
            encoder.setRenderPipelineState(pipelineState)
            encoder.setFragmentTexture(tex, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }

        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}

