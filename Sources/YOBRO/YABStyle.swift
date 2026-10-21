import SwiftUI
import AppKit

/// The window chrome takes the active Space's colour, Arc-style. Text and
/// icons on it use a dark shade of the same hue so the sidebar reads as one
/// tinted surface instead of grey controls on a coloured background.
enum SpaceTone: String, CaseIterable, Identifiable {
    case blue, peach, green, lilac, sand, rose, teal, slate
    var id: String { rawValue }

    var title: String {
        switch self {
        case .blue: return L("Blau", "Blue")
        case .peach: return L("Pfirsich", "Peach")
        case .green: return L("Grün", "Green")
        case .lilac: return L("Flieder", "Lilac")
        case .sand: return L("Sand")
        case .rose: return L("Rose")
        case .teal: return L("Petrol", "Teal")
        case .slate: return L("Schiefer", "Slate")
        }
    }

    /// Light tone, a dark shade of its hue for text and icons, and the dark
    /// appearance's counterparts. The shades are OKLCH(0.38, 0.066, hue) and
    /// OKLCH(0.80, 0.04, hue) of each tone.
    private var values: (light: UInt32, muted: UInt32, dark: UInt32, darkMuted: UInt32) {
        switch self {
        case .blue: return (0xA9BDEB, 0x324166, 0x232B3F, 0xB2BED8)
        case .peach: return (0xE8B9A5, 0x5F3624, 0x3A261D, 0xD3B7AB)
        case .green: return (0xB4D3AE, 0x2E4B29, 0x223020, 0xB2C4AE)
        case .lilac: return (0xC9B9EC, 0x473A61, 0x2E273D, 0xC1B9D4)
        case .sand: return (0xE6D4A4, 0x504012, 0x322B15, 0xC8BDA1)
        case .rose: return (0xEDB6C7, 0x5E3342, 0x3A242B, 0xD4B4BD)
        case .teal: return (0xA6D5D2, 0x004D4B, 0x1B302F, 0xA9C4C2)
        case .slate: return (0xC4C9D3, 0x3C4250, 0x292B30, 0xBBBEC4)
        }
    }

    var background: Color { YOBROTheme.adaptive("YAB.tone.\(rawValue)", light: values.light, dark: values.dark) }
    /// Default text and icon colour on the tone.
    var muted: Color { YOBROTheme.adaptive("YAB.muted.\(rawValue)", light: values.muted, dark: values.darkMuted) }
    /// Swatch for menus, always the light tone.
    var swatch: NSImage {
        let image = NSImage(size: NSSize(width: 14, height: 14))
        image.lockFocus()
        NSColor(srgbRed: CGFloat((values.light >> 16) & 255) / 255, green: CGFloat((values.light >> 8) & 255) / 255,
                blue: CGFloat(values.light & 255) / 255, alpha: 1).setFill()
        NSBezierPath(ovalIn: NSRect(x: 1, y: 1, width: 12, height: 12)).fill()
        image.unlockFocus()
        return image
    }

    static func automatic(at index: Int) -> SpaceTone { allCases[max(0, index) % allCases.count] }
}

/// Measurements and translucent layers of the tinted chrome.
enum Chrome {
    static let sidebarWidth: CGFloat = 228
    /// Gap between the window edge, the sidebar and the page cards.
    static let inset: CGFloat = 8
    static let radius: CGFloat = 10
    static let rowHeight: CGFloat = 36

    /// Resting fill for the address plate and pins.
    static let rest = overlay("YAB.rest", light: 0.34, dark: 0.07)
    static let restHover = overlay("YAB.restHover", light: 0.50, dark: 0.11)
    /// The selected row or pin.
    static let wash = overlay("YAB.wash", light: 0.62, dark: 0.15)
    static let hover = overlay("YAB.hover", light: 0.30, dark: 0.06)
    static let hairline = YOBROTheme.adaptiveAlpha("YAB.hairline", light: (0x171717, 0.08), dark: (0xFFFFFF, 0.10))

    private static func overlay(_ name: String, light: CGFloat, dark: CGFloat) -> Color {
        YOBROTheme.adaptiveAlpha(name, light: (0xFFFFFF, light), dark: (0xFFFFFF, dark))
    }
}

private struct SpaceToneKey: EnvironmentKey {
    static let defaultValue = SpaceTone.blue
}

extension EnvironmentValues {
    var spaceTone: SpaceTone {
        get { self[SpaceToneKey.self] }
        set { self[SpaceToneKey.self] = newValue }
    }
}

enum SidebarPreviewTiming {
    static let openDelay: UInt64 = 650_000_000
    static let closeDelay: UInt64 = 350_000_000
}

/// A square chrome button: back, forward, reload, the footer doors.
struct ChromeButtonStyle: ButtonStyle {
    var size: CGFloat = 30
    func makeBody(configuration: Configuration) -> some View {
        Surface(configuration: configuration, size: size)
    }
    private struct Surface: View {
        let configuration: ButtonStyle.Configuration
        let size: CGFloat
        @Environment(\.spaceTone) private var tone
        @Environment(\.isEnabled) private var enabled
        @State private var hovering = false
        var body: some View {
            configuration.label
                .font(.system(size: size > 28 ? 15 : 13))
                .foregroundStyle(tone.muted)
                .frame(width: size, height: size)
                .contentShape(Rectangle())
                .background(enabled && configuration.isPressed ? Chrome.wash : enabled && hovering ? Chrome.hover : .clear,
                            in: RoundedRectangle(cornerRadius: Chrome.radius))
                .opacity(enabled ? 1 : 0.4)
                .onHover { hovering = $0 }
        }
    }
}

/// Selection and hover fill of a sidebar row.
struct ChromeRowBackground: ViewModifier {
    let selected: Bool
    let hovering: Bool
    func body(content: Content) -> some View {
        content.background(selected ? Chrome.wash : hovering ? Chrome.hover : .clear,
                           in: RoundedRectangle(cornerRadius: Chrome.radius))
    }
}

/// White page card with a hairline ring, floating on the tinted window.
struct PageCard: ViewModifier {
    func body(content: Content) -> some View {
        content
            .clipShape(RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous)
                    .strokeBorder(Chrome.hairline, lineWidth: 1)
                    .allowsHitTesting(false)
            }
            .shadow(color: .black.opacity(0.10), radius: 2.5, y: 1)
    }
}

/// Dark rounded notice in the bottom corner, used for undo and downloads.
struct ToastSurface: ViewModifier {
    func body(content: Content) -> some View {
        content
            .font(.system(size: 12.5))
            .foregroundStyle(.white.opacity(0.96))
            .environment(\.colorScheme, .dark)
            .background(Color(white: 0.11, opacity: 0.9), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .shadow(color: .black.opacity(0.2), radius: 11, y: 8)
    }
}

extension View {
    func chromeRow(selected: Bool, hovering: Bool) -> some View { modifier(ChromeRowBackground(selected: selected, hovering: hovering)) }
    func pageCard() -> some View { modifier(PageCard()) }
    func toastSurface() -> some View { modifier(ToastSurface()) }
}
