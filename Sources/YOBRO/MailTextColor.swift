import AppKit

/// Keep the mail editor's appearance separate from the message's color model.
/// Colorless rich text otherwise uses AppKit's black attributed-string default,
/// rather than the adaptive color used for ordinary typing.
enum MailTextColor {
    static func applyAutomaticTextColor(to text: NSMutableAttributedString) {
        var changes: [(NSRange, NSColor)] = []
        text.enumerateAttributes(in: NSRange(location: 0, length: text.length)) { attributes, range, _ in
            guard let color = displayColor(for: attributes) else { return }
            if let current = attributes[.foregroundColor] as? NSColor, current.isEqual(color) { return }
            changes.append((range, color))
        }
        for (range, color) in changes { text.addAttribute(.foregroundColor, value: color, range: range) }
    }

    static func typingAttributes(_ attributes: [NSAttributedString.Key: Any]) -> [NSAttributedString.Key: Any] {
        var result = attributes
        if let color = displayColor(for: attributes) { result[.foregroundColor] = color }
        return result
    }

    /// Drafts and outgoing HTML retain automatic body color, never the white
    /// color resolved for the editor's dark appearance. Explicit accent colors
    /// and foreground/background combinations are preserved.
    static func contentForExport(_ source: NSAttributedString) -> NSAttributedString {
        let result = NSMutableAttributedString(attributedString: source)
        applyAutomaticTextColor(to: result)
        var automatic: [NSRange] = []
        result.enumerateAttribute(.foregroundColor, in: NSRange(location: 0, length: result.length)) { value, range, _ in
            if let color = value as? NSColor, color.isEqual(NSColor.textColor) { automatic.append(range) }
        }
        for range in automatic { result.removeAttribute(.foregroundColor, range: range) }
        return result
    }

    private static func displayColor(for attributes: [NSAttributedString.Key: Any]) -> NSColor? {
        // Respect intentionally styled blocks, such as dark type on a highlight
        // or white type on a dark background.
        if let background = attributes[.backgroundColor] as? NSColor {
            guard let rgb = background.usingColorSpace(.sRGB), rgb.alphaComponent == 0 else { return nil }
        }
        let current = attributes[.foregroundColor] as? NSColor
        if let current, !current.isEqual(NSColor.textColor), !isDefaultBlack(current) { return nil }
        return attributes[.link] == nil ? NSColor.textColor : NSColor.linkColor
    }

    private static func isDefaultBlack(_ color: NSColor) -> Bool {
        // Older pasted drafts often carry explicit plain black. Treat that
        // ordinary body color as automatic, without altering colored accents.
        guard color.type == .componentBased, let rgb = color.usingColorSpace(.sRGB) else { return false }
        return rgb.alphaComponent >= 0.999 && rgb.redComponent <= 0.001 &&
            rgb.greenComponent <= 0.001 && rgb.blueComponent <= 0.001
    }
}
