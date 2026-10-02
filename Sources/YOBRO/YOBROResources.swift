import Foundation

/// Locates SwiftPM's resource bundle without `Bundle.module`.
///
/// The generated accessor only checks next to the `.app` and the absolute
/// build directory, and traps when neither exists. Release builds keep the
/// bundle in Contents/Resources, and the build directory under /private/tmp
/// disappears after a restart, so `Bundle.module` crashed YoBro at launch.
/// Do not use `Bundle.module` anywhere in this target; a test enforces it.
enum YOBROResources {
    static let bundleName = "YOBRO_YOBRO.bundle"

    static let bundle: Bundle = {
        let code = Bundle(for: Finder.self)
        let candidates = [
            Bundle.main.resourceURL,                       // packaged app
            Bundle.main.bundleURL,                         // `swift run`
            code.resourceURL,
            code.bundleURL.deletingLastPathComponent(),    // `swift test`
        ]
        for directory in candidates.compactMap({ $0 }) {
            if let bundle = Bundle(url: directory.appendingPathComponent(bundleName)) { return bundle }
        }
        // Missing resources degrade single features; they must not end the app.
        return Bundle.main
    }()

    private final class Finder {}
}
