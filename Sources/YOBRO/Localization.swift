import Foundation

/// Use the primary system language, not a secondary preferred translation.
/// Stable identifiers and user content are deliberately kept out of localization.
enum YOBROLanguage {
    static func identifier(for preferredLanguages: [String]) -> String {
        let primary = preferredLanguages.first?.replacingOccurrences(of: "_", with: "-").lowercased() ?? "en"
        return primary.split(separator: "-").first == "de" ? "de" : "en"
    }

    static let identifier = identifier(for: Locale.preferredLanguages)
    static let isGerman = identifier == "de"
    static let locale = Locale(identifier: isGerman ? "de_DE" : "en_US")
    static let english: [String: String] = {
        guard let url = Bundle.module.url(forResource: "English", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let strings = try? JSONDecoder().decode([String: String].self, from: data) else { return [:] }
        return strings
    }()
}

func L(_ german: String) -> String {
    YOBROLanguage.isGerman ? german : YOBROLanguage.english[german] ?? german
}

/// Interpolated messages retain their values without using localized text as state.
func L(_ german: String, _ english: String) -> String {
    YOBROLanguage.isGerman ? german : english
}
