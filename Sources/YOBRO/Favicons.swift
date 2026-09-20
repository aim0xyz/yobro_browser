import AppKit
import WebKit

@MainActor
final class FaviconStore {
    static let shared = FaviconStore()
    private var cache: [String: NSImage] = [:]
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 8
        config.httpMaximumConnectionsPerHost = 2
        return URLSession(configuration: config)
    }()

    static func persistedData(for image: NSImage) -> Data? {
        let pixels = 64
        guard let bitmap = NSBitmapImageRep(
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
        ) else { return nil }
        bitmap.size = NSSize(width: pixels, height: pixels)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        NSColor.clear.setFill()
        NSRect(x: 0, y: 0, width: pixels, height: pixels).fill()
        image.draw(in: NSRect(x: 0, y: 0, width: pixels, height: pixels), from: .zero, operation: .sourceOver, fraction: 1)
        NSGraphicsContext.restoreGraphicsState()
        guard let data = bitmap.representation(using: .png, properties: [:]), data.count <= 256 * 1024 else { return nil }
        return data
    }

    func image(for page: URL) async -> NSImage? {
        guard ["http", "https"].contains(page.scheme ?? ""), let host = page.host else { return nil }
        let origin = "\(page.scheme!)://\(host)\(page.port.map { ":\($0)" } ?? "")"
        return await load(origin + "/favicon.ico")
    }

    func image(for webView: WKWebView) async -> NSImage? {
        guard let page = webView.url, ["http", "https"].contains(page.scheme ?? ""), let host = page.host else { return nil }
        let origin = "\(page.scheme!)://\(host)\(page.port.map { ":\($0)" } ?? "")"
        let icons = (try? await webView.evaluateJavaScript("Array.from(document.querySelectorAll('link[rel~=icon],link[rel=apple-touch-icon]')).map(x=>x.href).slice(0,8)")) as? [String] ?? []
        for address in icons + [origin + "/favicon.ico"] {
            if let image = await load(address) { return image }
        }
        return nil
    }

    private func load(_ address: String) async -> NSImage? {
        if let image = cache[address] { return image }
        guard let url = URL(string: address), ["http", "https"].contains(url.scheme ?? "") else { return nil }
        do {
            let (data, response) = try await session.data(from: url)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200,
                  data.count <= 1024 * 1024,
                  let image = NSImage(data: data) else { return nil }
            if cache.count > 250 { cache.removeAll() }
            cache[address] = image
            return image
        } catch { return nil }
    }
}
