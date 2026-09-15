import SwiftUI
import AppKit

@MainActor
final class RichTextCommands: ObservableObject {
    weak var textView: NSTextView?
    private var rememberedSelection = NSRange(location: 0, length: 0)

    func rememberSelection() { if let textView { rememberedSelection = textView.selectedRange() } }

    func toggleBold() { transformFont(.boldFontMask) }
    func toggleItalic() { transformFont(.italicFontMask) }

    func toggleUnderline() {
        guard let textView else { return }
        let range = effectiveRange(in: textView)
        let current = textView.textStorage?.attribute(.underlineStyle, at: max(0, min(range.location, max(0, textView.string.utf16.count - 1))), effectiveRange: nil) as? Int ?? 0
        apply(.underlineStyle, value: current == 0 ? NSUnderlineStyle.single.rawValue : 0, range: range, in: textView)
    }

    func heading() {
        guard let textView else { return }
        let range = paragraphRange(in: textView)
        textView.textStorage?.enumerateAttribute(.font, in: range) { value, subrange, _ in
            let font = (value as? NSFont) ?? .systemFont(ofSize: 15)
            let bold = NSFontManager.shared.convert(font, toHaveTrait: .boldFontMask)
            textView.textStorage?.addAttribute(.font, value: NSFont(descriptor: bold.fontDescriptor, size: 24) ?? bold, range: subrange)
        }
        textView.didChangeText()
    }

    func bodyText() {
        guard let textView else { return }
        let range = paragraphRange(in: textView)
        textView.textStorage?.addAttribute(.font, value: NSFont.systemFont(ofSize: 15), range: range)
        textView.didChangeText()
    }

    func addLink(_ rawURL: String) -> Bool {
        guard let textView,
              rememberedSelection.length > 0,
              let url = normalizedURL(rawURL) else { return false }
        let safe = NSIntersectionRange(rememberedSelection, NSRange(location: 0, length: textView.string.utf16.count))
        guard safe.length > 0 else { return false }
        textView.textStorage?.addAttributes([
            .link: url,
            .foregroundColor: NSColor.linkColor,
            .underlineStyle: NSUnderlineStyle.single.rawValue
        ], range: safe)
        textView.setSelectedRange(safe)
        textView.window?.makeFirstResponder(textView)
        textView.didChangeText()
        return true
    }

    private func normalizedURL(_ raw: String) -> URL? {
        let value = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else { return nil }
        let candidate = value.contains("://") ? value : "https://" + value
        guard let url = URL(string: candidate), ["http", "https"].contains(url.scheme?.lowercased() ?? ""), url.host != nil else { return nil }
        return url
    }

    private func transformFont(_ trait: NSFontTraitMask) {
        guard let textView else { return }
        let range = effectiveRange(in: textView)
        textView.textStorage?.enumerateAttribute(.font, in: range) { value, subrange, _ in
            let font = (value as? NSFont) ?? .systemFont(ofSize: 15)
            let manager = NSFontManager.shared
            let hasTrait = manager.traits(of: font).contains(trait)
            let converted = hasTrait ? manager.convert(font, toNotHaveTrait: trait) : manager.convert(font, toHaveTrait: trait)
            textView.textStorage?.addAttribute(.font, value: converted, range: subrange)
        }
        textView.didChangeText()
    }

    private func effectiveRange(in textView: NSTextView) -> NSRange {
        let selected = textView.selectedRange()
        if selected.length > 0 { return selected }
        textView.typingAttributes[.font] = textView.typingAttributes[.font] ?? NSFont.systemFont(ofSize: 15)
        return selected
    }

    private func paragraphRange(in textView: NSTextView) -> NSRange {
        let selected = textView.selectedRange()
        return (textView.string as NSString).paragraphRange(for: selected)
    }

    private func apply(_ key: NSAttributedString.Key, value: Any, range: NSRange, in textView: NSTextView) {
        if range.length == 0 { textView.typingAttributes[key] = value }
        else { textView.textStorage?.addAttribute(key, value: value, range: range); textView.didChangeText() }
    }
}

struct RichTextEditor: NSViewRepresentable {
    var rtf: Data?
    var fallbackText: String
    let commands: RichTextCommands
    let changed: (Data, String) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(changed: changed) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        guard let textView = scroll.documentView as? NSTextView else { return scroll }
        textView.isRichText = true
        textView.allowsUndo = true
        textView.isAutomaticLinkDetectionEnabled = true
        textView.font = .systemFont(ofSize: 15)
        textView.textContainerInset = NSSize(width: 18, height: 18)
        textView.backgroundColor = .clear
        textView.delegate = context.coordinator
        context.coordinator.install(rtf: rtf, fallbackText: fallbackText, in: textView)
        commands.textView = textView
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.changed = changed
        guard let textView = scroll.documentView as? NSTextView else { return }
        commands.textView = textView
        guard !context.coordinator.editing,
              context.coordinator.lastRTF != rtf else { return }
        context.coordinator.install(rtf: rtf, fallbackText: fallbackText, in: textView)
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var changed: (Data, String) -> Void
        var lastRTF: Data?
        var editing = false
        init(changed: @escaping (Data, String) -> Void) { self.changed = changed }

        func install(rtf: Data?, fallbackText: String, in textView: NSTextView) {
            editing = true
            if let rtf, let value = NSAttributedString(rtf: rtf, documentAttributes: nil) {
                textView.textStorage?.setAttributedString(value)
            } else {
                textView.string = fallbackText
                textView.font = .systemFont(ofSize: 15)
            }
            lastRTF = rtf
            editing = false
        }

        func textDidChange(_ notification: Notification) {
            guard !editing, let textView = notification.object as? NSTextView else { return }
            Self.clearOrphanedLinkTypingAttributes(in: textView)
            guard
                  let data = textView.attributedString().rtf(from: NSRange(location: 0, length: textView.string.utf16.count), documentAttributes: [:]) else { return }
            lastRTF = data
            changed(data, textView.string)
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard !editing, let textView = notification.object as? NSTextView else { return }
            Self.clearOrphanedLinkTypingAttributes(in: textView)
        }

        /// AppKit keeps the deleted run's typing attributes at the insertion
        /// point. Without clearing them, text entered after deleting a link is
        /// silently added to that no-longer-existing link.
        static func clearOrphanedLinkTypingAttributes(in textView: NSTextView) {
            let selection = textView.selectedRange()
            guard selection.length == 0 else { return }
            let typingAttributes = textView.typingAttributes
            let typingColor = typingAttributes[.foregroundColor] as? NSColor
            // AppKit's automatic link detector can remove `.link` while
            // leaving its blue color and underline behind. Treat either form
            // as link residue; an ordinary user-selected underline has no
            // link color and therefore remains untouched.
            let hasLinkResidue = typingAttributes[.link] != nil || typingColor?.isEqual(NSColor.linkColor) == true
            guard hasLinkResidue else { return }
            let storage = textView.textStorage
            let length = storage?.length ?? 0
            let cursor = min(selection.location, length)
            let linkedBefore = cursor > 0 && storage?.attribute(.link, at: cursor - 1, effectiveRange: nil) != nil
            let linkedAfter = cursor < length && storage?.attribute(.link, at: cursor, effectiveRange: nil) != nil
            guard !linkedBefore, !linkedAfter else { return }

            var attributes = typingAttributes
            attributes.removeValue(forKey: .link)
            attributes.removeValue(forKey: .underlineStyle)
            attributes[.foregroundColor] = NSColor.textColor
            textView.typingAttributes = attributes
        }
    }
}

struct RichTextToolbar: View {
    @ObservedObject var commands: RichTextCommands
    @State private var addingLink = false
    @State private var link = ""
    @State private var linkError = false

    var body: some View {
        HStack(spacing: 5) {
            Button { commands.heading() } label: { Text("H1").font(.system(size: 12, weight: .bold)) }
                .help(L("Überschrift", "Heading"))
            Button { commands.bodyText() } label: { Text("T").font(.system(size: 13)) }
                .help(L("Fließtext", "Body text"))
            Divider().frame(height: 18)
            Button { commands.toggleBold() } label: { Image(systemName: "bold") }
            Button { commands.toggleItalic() } label: { Image(systemName: "italic") }
            Button { commands.toggleUnderline() } label: { Image(systemName: "underline") }
            Button { commands.rememberSelection(); link = ""; linkError = false; addingLink = true } label: { Image(systemName: "link") }
                .help(L("Ausgewählten Text verlinken", "Link selected text"))
                .popover(isPresented: $addingLink) {
                    VStack(alignment: .leading, spacing: 10) {
                        Text(L("Link einfügen", "Insert link")).font(.headline)
                        TextField("https://…", text: $link).textFieldStyle(YOBROTextFieldStyle())
                        if linkError { Text(L("Markiere Text und gib eine gültige URL ein.", "Select text and enter a valid URL.")).font(.caption).foregroundStyle(.orange) }
                        HStack { Spacer(); Button(L("Einfügen", "Insert")) { if commands.addLink(link) { addingLink = false } else { linkError = true } }.keyboardShortcut(.defaultAction) }
                    }.padding(16).frame(width: 330)
                }
            Spacer()
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 30))
    }
}
