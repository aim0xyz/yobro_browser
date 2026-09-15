import SwiftUI

enum MailPasswordProvider {
    case google, apple
    static func detect(provider: String, account: MailAccount) -> Self? {
        if provider == "Gmail" { return .google }
        if provider == "iCloud Mail" { return .apple }
        guard provider == "Automatisch" || provider == "Manuell" else { return nil }
        let domain = account.address.trimmingCharacters(in: .whitespacesAndNewlines).lowercased().split(separator: "@").last.map(String.init) ?? ""
        let host = account.imapHost.lowercased()
        if ["gmail.com", "googlemail.com"].contains(domain) || host == "imap.gmail.com" { return .google }
        if ["icloud.com", "me.com", "mac.com"].contains(domain) || host == "imap.mail.me.com" { return .apple }
        return nil
    }
    var title: String { self == .google ? L("App-Passwort für Gmail erstellen") : L("App-Passwort für iCloud erstellen") }
    var steps: [String] {
        self == .google ? [
            L("Aktiviere die Bestätigung in zwei Schritten in deinem Google-Konto."),
            L("Öffne über den Link unten die App-Passwörter, gib als Namen „YoBro“ ein und erstelle ein Passwort."),
            L("Kopiere den angezeigten 16-stelligen Code in das Passwortfeld oben und verbinde dein Postfach.")
        ] : [
            L("Aktiviere die Zwei-Faktor-Authentifizierung für deinen Apple Account."),
            L("Melde dich über den Link unten an. Öffne „Anmelden und Sicherheit“ → „App-spezifische Passwörter“ → „App-spezifisches Passwort erstellen“ und verwende den Namen „YoBro“."),
            L("Kopiere das erstellte Passwort in das Passwortfeld oben und verbinde dein Postfach.")
        ]
    }
    var createURL: URL { URL(string: self == .google ? "https://myaccount.google.com/apppasswords" : "https://account.apple.com/")! }
    var helpURL: URL { URL(string: self == .google ? "https://support.google.com/accounts/answer/185833?hl=\(YOBROLanguage.identifier)" : "https://support.apple.com/\(YOBROLanguage.isGerman ? "de-de" : "en-us")/102654")! }
}

struct MailPasswordHelp: View {
    let provider: MailPasswordProvider
    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            Label(provider.title, systemImage: "key.horizontal").font(.system(size: 12, weight: .semibold))
            ForEach(Array(provider.steps.enumerated()), id: \.offset) { index, step in
                HStack(alignment: .top, spacing: 7) {
                    Text("\(index + 1).").foregroundStyle(moss)
                    Text(step).fixedSize(horizontal: false, vertical: true)
                }.font(.system(size: 11)).lineSpacing(2)
            }
            if provider == .google {
                Text(L("Fehlt die Option? Bei Firmen-/Schulkonten, erweitertem Schutz oder bestimmten Sicherheitsschlüssel-Einstellungen sind App-Passwörter möglicherweise nicht verfügbar."))
                    .font(.system(size: 10)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 16) {
                Link(L("App-Passwort erstellen ↗"), destination: provider.createURL)
                Link(L("Offizielle Anleitung ↗"), destination: provider.helpURL)
            }.font(.system(size: 11, weight: .medium)).tint(moss)
        }.padding(13).frame(maxWidth: .infinity, alignment: .leading).background(moss.opacity(0.07), in: RoundedRectangle(cornerRadius: 12))
    }
}
