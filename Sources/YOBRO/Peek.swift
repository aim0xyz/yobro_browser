import SwiftUI
import WebKit

/// The previewed page floats above a dimmed page card, with its controls
/// beside it. `peek`, `closePeek` and `keepPeek` live in BrowserModel.swift,
/// next to the tab selection they depend on.
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
            // A page dialog over the preview takes Esc first.
            if model.webPageDialog == nil {
                Button("") { model.closePeek() }.keyboardShortcut(.cancelAction).opacity(0).allowsHitTesting(false)
            }
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
