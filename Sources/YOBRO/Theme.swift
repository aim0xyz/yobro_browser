import SwiftUI
#if os(macOS)
import AppKit
#else
import UIKit
#endif

// Shared desktop/iOS palette, resolved against the current system appearance.
enum YOBROTheme {
    static func adaptive(_ name: String, light: UInt32, dark: UInt32) -> Color {
        #if os(macOS)
        Color(nsColor: NSColor(name: NSColor.Name(name)) { appearance in
            let value = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
            return NSColor(srgbRed: CGFloat((value >> 16) & 255) / 255,
                           green: CGFloat((value >> 8) & 255) / 255,
                           blue: CGFloat(value & 255) / 255, alpha: 1)
        })
        #else
        Color(uiColor: UIColor { traits in
            let value = traits.userInterfaceStyle == .dark ? dark : light
            return UIColor(red: CGFloat((value >> 16) & 255) / 255,
                           green: CGFloat((value >> 8) & 255) / 255,
                           blue: CGFloat(value & 255) / 255, alpha: 1)
        })
        #endif
    }
    static let text = adaptive("YOBRO.text", light: 0x303B3B, dark: 0xE3E9E0)
    static let accent = adaptive("YOBRO.accent", light: 0x4F6654, dark: 0xADC6A8)
    static let page = adaptive("YOBRO.page", light: 0xF9F8F4, dark: 0x191E1B)
    static let surface = adaptive("YOBRO.surface", light: 0xFFFFFF, dark: 0x39433B)
    static let chromeTop = adaptive("YOBRO.chromeTop", light: 0xE3E6D6, dark: 0x252E28)
    static let chromeBottom = adaptive("YOBRO.chromeBottom", light: 0xD6DED4, dark: 0x1B241F)
    static let sage = adaptive("YOBRO.sage", light: 0xDBE6D4, dark: 0x344735)
    static let lilac = adaptive("YOBRO.lilac", light: 0xE0DBEB, dark: 0x40384E)
    static let peach = adaptive("YOBRO.peach", light: 0xF2DECC, dark: 0x513E30)
    static let folderSage = adaptive("YOBRO.folderSage", light: 0x71896F, dark: 0xA7C1A3)
    static let folderLilac = adaptive("YOBRO.folderLilac", light: 0x887CA0, dark: 0xBAACCD)
    static let folderPeach = adaptive("YOBRO.folderPeach", light: 0xB47E5D, dark: 0xD6A07D)
    static let folderSky = adaptive("YOBRO.folderSky", light: 0x66888E, dark: 0x92B9BF)
    static let folderSand = adaptive("YOBRO.folderSand", light: 0x9D8756, dark: 0xCDB77D)
    static let folderRose = adaptive("YOBRO.folderRose", light: 0x997078, dark: 0xC79AA3)
    static let field = adaptive("YOBRO.field", light: 0xF4F5EF, dark: 0x303A33)
    static let border = adaptive("YOBRO.border", light: 0xCAD3C8, dark: 0x536157)
    static let brandOrange = adaptive("YOBRO.brandOrange", light: 0xFF541C, dark: 0xFF6A38)
}

#if os(macOS)
let ink = YOBROTheme.text
let moss = YOBROTheme.accent
let paper = YOBROTheme.page
let brandOrange = YOBROTheme.brandOrange

enum FolderColor: String, CaseIterable, Identifiable {
    case moss, sage, lilac, peach, sky, sand, rose
    var id: String { rawValue }
    var title: String {
        switch self {
        case .moss: return L("Moos", "Moss")
        case .sage: return L("Salbei", "Sage")
        case .lilac: return L("Flieder", "Lilac")
        case .peach: return L("Pfirsich", "Peach")
        case .sky: return L("Himmel", "Sky")
        case .sand: return L("Sand")
        case .rose: return L("Rose")
        }
    }
    var color: Color {
        switch self {
        case .moss: return YOBROTheme.accent
        case .sage: return YOBROTheme.folderSage
        case .lilac: return YOBROTheme.folderLilac
        case .peach: return YOBROTheme.folderPeach
        case .sky: return YOBROTheme.folderSky
        case .sand: return YOBROTheme.folderSand
        case .rose: return YOBROTheme.folderRose
        }
    }
    var menuImage: NSImage {
        let image = NSImage(size: NSSize(width: 14, height: 14))
        image.lockFocus()
        NSColor(color).setFill()
        NSBezierPath(ovalIn: NSRect(x: 1, y: 1, width: 12, height: 12)).fill()
        image.unlockFocus()
        image.isTemplate = false
        return image
    }
}

enum SpaceIcon: String, CaseIterable, Identifiable {
    case layers = "square.stack.3d.up.fill"
    case home = "house.fill"
    case work = "briefcase.fill"
    case creative = "paintpalette.fill"
    case travel = "airplane"
    case learning = "graduationcap.fill"
    case favorite = "heart.fill"
    case ideas = "sparkles"
    case nature = "leaf.fill"
    case making = "hammer.fill"
    case music = "music.note"
    case reading = "book.fill"
    case shopping = "cart.fill"
    var id: String { rawValue }
    var title: String {
        switch self {
        case .layers: return L("Ebenen", "Layers")
        case .home: return L("Zuhause", "Home")
        case .work: return L("Arbeit", "Work")
        case .creative: return L("Kreativ", "Creative")
        case .travel: return L("Reisen", "Travel")
        case .learning: return L("Lernen", "Learning")
        case .favorite: return L("Favorit", "Favorite")
        case .ideas: return L("Ideen", "Ideas")
        case .nature: return L("Natur", "Nature")
        case .making: return L("Projekte", "Projects")
        case .music: return L("Musik", "Music")
        case .reading: return L("Lesen", "Reading")
        case .shopping: return L("Einkaufen", "Shopping")
        }
    }
}

struct YOBROPopupButtonStyle: ButtonStyle {
    var prominent = false
    var color: Color = moss
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .semibold))
            .foregroundStyle(prominent ? YOBROTheme.page : ink)
            .padding(.horizontal, 14).frame(minHeight: 34)
            .background(
                prominent ? color.opacity(configuration.isPressed ? 0.76 : 1) : YOBROTheme.surface.opacity(configuration.isPressed ? 0.44 : 0.72),
                in: RoundedRectangle(cornerRadius: 9)
            )
            .overlay(RoundedRectangle(cornerRadius: 9).stroke(prominent ? .clear : YOBROTheme.border.opacity(0.48), lineWidth: 1))
    }
}

struct YOBRONameEditor: View {
    let icon: String
    let title: String
    let detail: String
    @Binding var text: String
    let cancel: () -> Void
    let save: () -> Void
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 11) {
                Image(systemName: icon).font(.system(size: 14, weight: .semibold)).foregroundStyle(moss)
                    .frame(width: 34, height: 34).background(moss.opacity(0.13), in: RoundedRectangle(cornerRadius: 9))
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.system(size: 15, weight: .semibold, design: .rounded))
                    Text(detail).font(.system(size: 10)).foregroundStyle(.secondary)
                }
            }
            TextField(L("Name"), text: $text)
                .textFieldStyle(YOBROTextFieldStyle()).tint(moss).focused($focused)
                .onSubmit { if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { save() } }
            HStack(spacing: 9) {
                Spacer()
                Button(L("Abbrechen"), action: cancel).buttonStyle(YOBROPopupButtonStyle())
                Button(L("Speichern"), action: save).buttonStyle(YOBROPopupButtonStyle(prominent: true))
                    .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(18).frame(width: 330)
        .background(paper)
        .onAppear { focused = true }
        .onExitCommand(perform: cancel)
    }
}

struct YOBROTextFieldStyle: TextFieldStyle {
    func _body(configuration: TextField<Self._Label>) -> some View {
        configuration
            .textFieldStyle(.plain)
            .padding(.horizontal, 11).padding(.vertical, 8)
            .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 9))
            .overlay(RoundedRectangle(cornerRadius: 9).stroke(YOBROTheme.border.opacity(0.65), lineWidth: 1))
    }
}

struct YOBROCardModifier: ViewModifier {
    var padding: CGFloat
    var emphasized: Bool
    func body(content: Content) -> some View {
        content
            .padding(padding)
            .background(emphasized ? moss.opacity(0.1) : YOBROTheme.surface.opacity(0.58), in: RoundedRectangle(cornerRadius: 13))
            .overlay(RoundedRectangle(cornerRadius: 13).stroke(YOBROTheme.border.opacity(0.32), lineWidth: 1))
    }
}

extension View {
    func yobroCard(padding: CGFloat = 14, emphasized: Bool = false) -> some View {
        modifier(YOBROCardModifier(padding: padding, emphasized: emphasized))
    }
}

struct YOBROSettingsHeading: View {
    let icon: String
    let title: String
    let detail: String
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon).font(.system(size: 16, weight: .semibold)).foregroundStyle(moss)
                .frame(width: 36, height: 36).background(moss.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.system(size: 16, weight: .semibold, design: .rounded))
                Text(detail).font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}

#endif
