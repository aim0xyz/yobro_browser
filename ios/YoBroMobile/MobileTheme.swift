import SwiftUI

/// Uses the original desktop artwork without recoloring or redrawing it.
struct MobileBrandMark: View {
    var size: CGFloat
    private static let artwork: UIImage? = {
        guard let url = Bundle.main.url(forResource: "YOBROMark", withExtension: "png") else { return nil }
        return UIImage(contentsOfFile: url.path)
    }()
    var body: some View {
        Group {
            if let artwork = Self.artwork {
                Image(uiImage: artwork).resizable().interpolation(.high).scaledToFit()
            }
        }.frame(width: size, height: size).accessibilityHidden(true)
    }
}

struct MobileSurface: ViewModifier {
    func body(content: Content) -> some View {
        content
            .scrollContentBackground(.hidden)
            .background(YOBROTheme.page)
            .foregroundStyle(YOBROTheme.text)
            .tint(YOBROTheme.accent)
            .toolbarBackground(YOBROTheme.page, for: .navigationBar)
            .toolbarBackground(.visible, for: .navigationBar)
    }
}
extension View {
    func mobileSurface() -> some View { modifier(MobileSurface()) }
}
