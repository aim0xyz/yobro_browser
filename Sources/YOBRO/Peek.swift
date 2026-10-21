import SwiftUI
import WebKit

extension BrowserModel {
    /// Opens a link in a preview over the current page, like YAB's Peek.
    /// Nothing reaches the sidebar until the user keeps the page.
    func peek(_ url: URL) {
        closePeek()
        let tab = BrowserTab(saved: StoredTab(id: UUID(), title: url.host ?? url.absoluteString, url: "", space: space, pinned: false), owner: self)
        peekTab = tab
        do { try tab.navigate(url.absoluteString) }
        catch { closePeek(); notice = error.localizedDescription }
    }

    func closePeek() {
        guard let tab = peekTab else { return }
        peekTab = nil
        tab.discard()
    }

    /// Keeps the previewed page as an ordinary tab, optionally beside the
    /// page it was opened from.
    func keepPeek(split: Bool) {
        guard let tab = peekTab else { return }
        peekTab = nil
        tabs.append(tab)
        if #available(macOS 15.4, *) { extensions.runtime.controller.didOpenTab(tab) }
        if split, let anchor = activeID, anchor != tab.id, pairTabs(tab.id, with: anchor) { return }
        select(tab.id)
        save()
    }
}

/// The previewed page floats above a dimmed page card, with its controls
/// beside it.
struct PeekOverlay: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous)
                .fill(.black.opacity(0.16))
                .onTapGesture { model.closePeek() }
            HStack(alignment: .top, spacing: 10) {
                TabContent(tab: tab, model: model)
                    .background(paper)
                    .clipShape(RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous).strokeBorder(Chrome.hairline))
                    .shadow(color: .black.opacity(0.14), radius: 12, y: 8)
                VStack(spacing: 10) {
                    knob("xmark", L("Schließen · Esc", "Close · Esc")) { model.closePeek() }
                    knob("arrow.up.left.and.arrow.down.right", L("Als Tab öffnen", "Open as tab")) { model.keepPeek(split: false) }
                    knob("rectangle.split.2x1", L("Daneben öffnen", "Open side by side")) { model.keepPeek(split: true) }
                }
            }
            .padding(.leading, 48).padding(.trailing, 12).padding(.vertical, 24)
        }
        .background {
            Button("") { model.closePeek() }.keyboardShortcut(.cancelAction).opacity(0).allowsHitTesting(false)
        }
        .transition(.opacity.combined(with: .scale(scale: 0.98)))
        .accessibilityIdentifier("peek")
    }

    private func knob(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 11, weight: .semibold))
                .frame(width: 28, height: 28)
                .background(paper, in: Circle())
                .shadow(color: .black.opacity(0.18), radius: 6, y: 2)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(ink.opacity(0.75))
        .yobroHelp(help)
        .accessibilityLabel(help)
    }
}
