import SwiftUI
import WebKit

/// What a page is doing that the tab chrome should show: camera and
/// microphone capture from WebKit, plus audible playback and the hovered link
/// reported by PageSignals.js.
@MainActor
final class PageSignals: ObservableObject {
    @Published private(set) var camera = false
    @Published private(set) var microphone = false
    @Published private(set) var audible = false
    @Published private(set) var hoveredLink = ""
    private weak var webView: WKWebView?
    private var observers: [NSKeyValueObservation] = []
    /// Documents, frames included, that currently play sound.
    private var audibleDocuments: Set<String> = []

    init(webView: WKWebView) {
        self.webView = webView
        camera = webView.cameraCaptureState != .none
        microphone = webView.microphoneCaptureState != .none
        observers = [
            webView.observe(\.cameraCaptureState, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in self?.camera = view.cameraCaptureState != .none }
            },
            webView.observe(\.microphoneCaptureState, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in self?.microphone = view.microphoneCaptureState != .none }
            },
        ]
    }

    func stopCapture() {
        webView?.setCameraCaptureState(.none, completionHandler: nil)
        webView?.setMicrophoneCaptureState(.none, completionHandler: nil)
    }

    /// The pause events that follow clear `audible` through the page script.
    func pauseAudio() {
        webView?.pauseAllMediaPlayback(completionHandler: nil)
    }

    func setHoveredLink(_ href: String) {
        // A script URL says nothing useful about where a click leads.
        let link = href.lowercased().hasPrefix("javascript:") ? "" : href
        if hoveredLink != link { hoveredLink = link }
    }

    func setAudible(_ playing: Bool, document: String) {
        if playing {
            // A page cannot grow this without bound by inventing documents.
            guard audibleDocuments.count < 64 else { return }
            audibleDocuments.insert(document)
        } else {
            audibleDocuments.remove(document)
        }
        if audible != !audibleDocuments.isEmpty { audible = !audibleDocuments.isEmpty }
    }
}

enum PageSignalsScript {
    static let world = WKContentWorld.world(name: "YOBRO.Signals")
    static let handlerName = "yobroSignals"
    private static let source: String? = YOBROResources.bundle.url(forResource: "PageSignals", withExtension: "js")
        .flatMap { try? String(contentsOf: $0, encoding: .utf8) }

    @MainActor
    static func install(on controller: WKUserContentController) {
        guard let source else { return }
        // WebKit raises an Objective-C exception for a duplicate handler name.
        controller.removeScriptMessageHandler(forName: handlerName, contentWorld: world)
        controller.add(PageSignalsHandler.shared, contentWorld: world, name: handlerName)
        guard !controller.userScripts.contains(where: { $0.source == source }) else { return }
        controller.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: world))
    }
}

/// One stateless handler for every controller. The controller retains its
/// handlers, so a per-tab handler would keep the tab alive.
@MainActor
final class PageSignalsHandler: NSObject, WKScriptMessageHandler {
    static let shared = PageSignalsHandler()
    static let linkLimit = 2048

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        route(message.body, from: message.webView)
    }

    /// Only YoBro's own tabs carry signals; any other web view is ignored.
    func route(_ body: Any, from webView: WKWebView?) {
        guard let signals = (webView as? AppearanceWebView)?.signals,
              let body = body as? [String: Any], let kind = body["kind"] as? String else { return }
        switch kind {
        case "link":
            guard let href = body["href"] as? String else { return }
            signals.setHoveredLink(String(href.prefix(Self.linkLimit)))
        case "audio":
            guard let document = body["doc"] as? String, !document.isEmpty, document.count <= 64,
                  let audible = body["audible"] as? Bool else { return }
            signals.setAudible(audible, document: document)
        default:
            break
        }
    }
}

/// Tab-row controls for a page that uses the camera or microphone or plays
/// sound. They stay visible without hover, like Safari's.
struct MediaIndicator: View {
    @ObservedObject var signals: PageSignals

    var body: some View {
        HStack(spacing: 0) {
            if signals.camera || signals.microphone {
                Button { signals.stopCapture() } label: {
                    Image(systemName: signals.camera ? "video.fill" : "mic.fill")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.red)
                }
                .yobroIconHelp(L("Kamera/Mikrofon beenden", "Stop camera/microphone"))
            }
            if signals.audible {
                Button { signals.pauseAudio() } label: { Image(systemName: "speaker.wave.2").font(.system(size: 11, weight: .medium)) }
                    .yobroIconHelp(L("Wiedergabe pausieren", "Pause playback"))
            }
        }
        .buttonStyle(ChromeButtonStyle(size: 22))
    }
}

/// A red dot on a favicon while the page captures camera or microphone.
struct CaptureDot: View {
    @ObservedObject var signals: PageSignals

    var body: some View {
        if signals.camera || signals.microphone {
            Circle()
                .fill(.red)
                .frame(width: 7, height: 7)
                .overlay(Circle().strokeBorder(.white.opacity(0.9), lineWidth: 1))
                .allowsHitTesting(false)
                .accessibilityLabel(signals.camera ? L("Kamera aktiv", "Camera in use") : L("Mikrofon aktiv", "Microphone in use"))
        }
    }
}

/// The destination of the link under the pointer or keyboard focus, shown
/// in the corner of the page.
struct HoveredLinkPill: View {
    @ObservedObject var signals: PageSignals

    var body: some View {
        ZStack(alignment: .leading) {
            if !signals.hoveredLink.isEmpty {
                Text(signals.hoveredLink)
                    .font(.system(size: 11.5))
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .toastSurface()
                    .transition(.opacity)
            }
        }
        // The surface hugs the text; only its width is capped.
        .frame(maxWidth: 480, alignment: .leading)
        .animation(.easeOut(duration: 0.12), value: signals.hoveredLink.isEmpty)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}
