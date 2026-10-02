import Foundation

/// Converts the agent's display answer into text that sounds natural when spoken.
enum BroSpeechText {
    static let laughAudioLimit: TimeInterval = 4

    static func prepare(_ text: String) -> String {
        let parsed = try? AttributedString(
            markdown: text,
            options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        )
        var spoken = parsed.map { String($0.characters) } ?? text
        spoken = spoken.replacingOccurrences(
            of: #"(?m)^[ \t]{0,3}(?:#{1,6}[ \t]+|[-*+][ \t]+|\d+[.)][ \t]+|>[ \t]*)"#,
            with: "",
            options: .regularExpression
        )
        spoken = spoken.replacingOccurrences(of: "*", with: "")
        spoken = spoken.replacingOccurrences(of: "`", with: "")
        spoken = spoken.replacingOccurrences(
            of: #"(?i)\[(?:lacht|laughs?|kichert|chuckles?)[^\]]*\]"#,
            with: "Haha.",
            options: .regularExpression
        )
        spoken = spoken.replacingOccurrences(
            of: #"(?i)\b(?:(?:ha){2,}|(?:he){2,}|(?:hi){3,}|(?:ho){3,}|(?:lol){2,})\b|[😂🤣😆]+"#,
            with: "Haha.",
            options: .regularExpression
        )
        spoken = spoken.replacingOccurrences(of: #"(?:Haha\.[ \t]*){2,}"#, with: "Haha. ", options: .regularExpression)
        spoken = spoken.replacingOccurrences(of: #"Haha\.[,.!?]+"#, with: "Haha.", options: .regularExpression)
        return spoken.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func utterances(in text: String) -> [String] {
        var result: [String] = []
        var remainder = text[...]
        while let laugh = remainder.range(of: "Haha.") {
            let before = remainder[..<laugh.lowerBound].trimmingCharacters(in: .whitespacesAndNewlines)
            if !before.isEmpty { result.append(before) }
            result.append("Haha.")
            remainder = remainder[laugh.upperBound...]
        }
        let after = remainder.trimmingCharacters(in: .whitespacesAndNewlines)
        if !after.isEmpty { result.append(after) }
        return result
    }
}
