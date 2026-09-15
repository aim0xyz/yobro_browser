import AppKit

let root = CommandLine.arguments[1]
let rootURL = URL(fileURLWithPath: root)
let folder = rootURL.appendingPathComponent("YOBRO.iconset")
let sourceURL = rootURL.appendingPathComponent("YOBRO-logo-source.png")

guard let source = NSImage(contentsOf: sourceURL) else {
    fatalError("Could not load exact YOBRO artwork at \(sourceURL.path)")
}

try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)

func render(pixels: Int) -> Data {
    let p = CGFloat(pixels)
    let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: pixels,
        pixelsHigh: pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    )!
    let context = NSGraphicsContext(bitmapImageRep: bitmap)!
    let previous = NSGraphicsContext.current
    NSGraphicsContext.current = context
    context.shouldAntialias = true

    NSColor.clear.setFill()
    NSRect(x: 0, y: 0, width: p, height: p).fill()
    source.draw(
        in: NSRect(x: 0, y: 0, width: p, height: p),
        from: .zero,
        operation: .sourceOver,
        fraction: 1,
        respectFlipped: true,
        hints: [.interpolation: NSImageInterpolation.high]
    )

    context.flushGraphics()
    NSGraphicsContext.current = previous
    return bitmap.representation(using: .png, properties: [:])!
}

for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let data = render(pixels: pixels)
        let name = "icon_\(size)x\(size)\(scale == 2 ? "@2x" : "").png"
        try data.write(to: folder.appendingPathComponent(name))
        if pixels == 1024 {
            try data.write(to: rootURL.appendingPathComponent("YOBRO-logo-master.png"))
        }
    }
}
