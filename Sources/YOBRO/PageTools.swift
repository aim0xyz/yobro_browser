import SwiftUI
import WebKit

/// A small, local alternative to a fingerprinting extension.  Detection only
/// inspects the document already loaded in the active tab; it does not send a
/// URL, HTML, or page metadata anywhere.
@MainActor
final class PageTechnologyInspector: ObservableObject {
    @Published private(set) var technologies: [String] = []
    @Published private(set) var isInspecting = false
    @Published var error: String?

    func inspect(_ webView: WKWebView) async {
        isInspecting = true
        error = nil
        defer { isInspecting = false }
        do {
            let value = try await webView.callAsyncJavaScript("""
                const found = new Set();
                const html = document.documentElement?.innerHTML || '';
                const scripts = Array.from(document.scripts, script => script.src || script.textContent || '').join('\\n');
                const has = value => html.includes(value) || scripts.includes(value);
                if (window.React || has('__NEXT_DATA__') || has('_next/')) found.add('React / Next.js');
                if (window.__VUE__ || has('__NUXT__') || has('_nuxt/')) found.add('Vue / Nuxt');
                if (window.angular || has('ng-version')) found.add('Angular');
                if (has('svelte') || has('_app/immutable')) found.add('Svelte');
                if (has('wp-content') || has('wordpress')) found.add('WordPress');
                if (has('shopify') || has('cdn.shopify.com')) found.add('Shopify');
                if (has('google-analytics.com') || has('googletagmanager.com')) found.add('Google Analytics');
                if (has('plausible.io')) found.add('Plausible Analytics');
                if (has('sentry.io')) found.add('Sentry');
                if (has('cloudflare')) found.add('Cloudflare');
                return Array.from(found).sort();
                """, arguments: [:], in: nil, contentWorld: .page)
            technologies = value as? [String] ?? []
        } catch {
            self.error = error.localizedDescription
        }
    }
}

struct PageToolsButton: View {
    @ObservedObject var tab: BrowserTab
    @StateObject private var inspector = PageTechnologyInspector()
    @State private var presented = false

    var body: some View {
        Button { presented.toggle() } label: {
            Image(systemName: "wrench.and.screwdriver")
                .frame(width: 38, height: 40)
        }
        .yobroHelp(L("Seitentools", "Page tools"))
        .accessibilityLabel(L("Seitentools", "Page tools"))
        .popover(isPresented: $presented, arrowEdge: .top) {
            VStack(alignment: .leading, spacing: 14) {
                Text(L("Seitentools", "Page tools")).font(.headline)
                Label(L("Web Inspector ist für diese Seite aktiviert. Rechtsklick in die Seite → „Element untersuchen“.", "Web Inspector is enabled for this page. Right-click the page → Inspect Element."), systemImage: "chevron.left.forwardslash.chevron.right")
                    .font(.system(size: 11)).fixedSize(horizontal: false, vertical: true)
                Divider()
                HStack {
                    Text(L("Erkannte Technik", "Detected technology")).font(.system(size: 12, weight: .semibold))
                    Spacer()
                    Button(L("Prüfen", "Scan")) { Task { await inspector.inspect(tab.webView) } }
                        .disabled(inspector.isInspecting)
                }
                if inspector.isInspecting { ProgressView().controlSize(.small) }
                else if let error = inspector.error { Text(error).font(.caption).foregroundStyle(.red) }
                else if inspector.technologies.isEmpty {
                    Text(L("Noch keine Treffer. Die Prüfung läuft ausschließlich lokal in dieser Seite.", "No matches yet. Scanning runs locally in this page only."))
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    ForEach(inspector.technologies, id: \.self) { technology in
                        Label(technology, systemImage: "checkmark.circle.fill").font(.system(size: 12)).foregroundStyle(moss)
                    }
                }
                Divider()
                Text(L("Übersetzung wird erst aktiv, wenn ein lokales Sprachpaket eingerichtet ist. Seiteninhalt wird bis dahin nicht an einen Übersetzungsdienst gesendet.", "Translation remains off until a local language pack is configured. Until then page content is never sent to a translation service."))
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            .padding(16).frame(width: 330).background(paper).foregroundStyle(ink)
            .task { await inspector.inspect(tab.webView) }
        }
    }
}
