import Foundation

struct MailDiscoveryResult {
    var account: MailAccount
    var explanation: String
    var oauthOnly = false
    var source: String
}

enum MailDiscovery {
    static func preset(_ name: String, address: String, id: UUID) -> MailDiscoveryResult {
        var account = MailAccount(id: id, label: name, address: address, username: address)
        var note = L("Passwort oder App-Passwort deines Mail-Anbieters verwenden.")
        var oauth = false
        switch name {
        case "Gmail":
            account.imapHost = "imap.gmail.com"; account.smtpHost = "smtp.gmail.com"
            note = L("Gmail: App-Passwort erforderlich (2-Faktor-Anmeldung). Falls dein Konto keine App-Passwörter erlaubt, wird OAuth benötigt; die Google-Anmeldung ist in dieser Vorschau noch nicht eingerichtet.")
        case "iCloud Mail":
            account.imapHost = "imap.mail.me.com"; account.smtpHost = "smtp.mail.me.com"; account.smtpPort = 587; account.smtpSecurity = "starttls"
            note = L("iCloud Mail: ein anwendungsspezifisches Passwort deines Apple Accounts verwenden. Apple Mail selbst ist ein Mailprogramm, kein Postfachanbieter.")
        case "Spacemail":
            account.imapHost = "mail.spacemail.com"; account.smtpHost = "mail.spacemail.com"
            note = L("Spacemail: vollständige E-Mail-Adresse und Postfachpasswort verwenden. Die Serverdaten funktionieren auch bei eigenen Domains.")
        case "Outlook / Microsoft 365":
            account.imapHost = "outlook.office365.com"; account.smtpHost = "smtp-mail.outlook.com"; account.smtpPort = 587; account.smtpSecurity = "starttls"
            note = L("Microsoft verlangt moderne Anmeldung per OAuth. Diese Vorschau hat noch keine registrierte Microsoft-Anmeldung; dieses Konto kann derzeit nicht verbunden werden.")
            oauth = true
        default: account.label = L("Mein Postfach")
        }
        return MailDiscoveryResult(account: account, explanation: note, oauthOnly: oauth, source: L("Anbietervorlage: ") + name)
    }

    static func discover(address: String, id: UUID) async throws -> MailDiscoveryResult {
        let parts = address.split(separator: "@", omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty else { throw YOBROError.message(L("Bitte eine gültige E-Mail-Adresse eingeben.")) }
        let domain = String(parts[1]).lowercased()
        guard domain.range(of: #"^[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z]{2,}$"#, options: .regularExpression) != nil else { throw YOBROError.message(L("Ungültige E-Mail-Domain.")) }
        if ["gmail.com", "googlemail.com"].contains(domain) { return preset("Gmail", address: address, id: id) }
        if ["icloud.com", "me.com", "mac.com"].contains(domain) { return preset("iCloud Mail", address: address, id: id) }
        if ["outlook.com", "hotmail.com", "live.com", "outlook.de"].contains(domain) { return preset("Outlook / Microsoft 365", address: address, id: id) }
        let endpoints = ["https://autoconfig.\(domain)/mail/config-v1.1.xml", "https://\(domain)/.well-known/autoconfig/mail/config-v1.1.xml", "https://autoconfig.thunderbird.net/v1.1/\(domain)"]
        for endpoint in endpoints {
            try Task.checkCancellation()
            do {
                let request = URLRequest(url: URL(string: endpoint)!, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 6)
                let (data, response) = try await URLSession.shared.data(for: request)
                guard let response = response as? HTTPURLResponse, response.statusCode == 200, response.url?.scheme == "https", data.count <= 262144 else { continue }
                let parser = ProviderXML(); let xml = XMLParser(data: data); xml.shouldResolveExternalEntities = false; xml.delegate = parser
                guard xml.parse(), let incoming = parser.servers.first(where: { $0["type"] == "imap" && $0["socketType"] == "SSL" }),
                      let outgoing = parser.servers.first(where: { $0["type"] == "smtp" && ["SSL", "STARTTLS"].contains($0["socketType"] ?? "") }),
                      let ih = incoming["hostname"], let sh = outgoing["hostname"], let ip = Int(incoming["port"] ?? ""), let sp = Int(outgoing["port"] ?? "") else { continue }
                let username = (incoming["username"] ?? "%EMAILADDRESS%").replacingOccurrences(of: "%EMAILADDRESS%", with: address).replacingOccurrences(of: "%EMAILLOCALPART%", with: String(parts[0])).replacingOccurrences(of: "%EMAILDOMAIN%", with: domain)
                let auth = (incoming["auth"] ?? "") + "," + (outgoing["auth"] ?? "")
                let incomingOnly = (incoming["auth"] ?? "").lowercased().split(separator: ",").allSatisfy { $0 == "oauth2" }
                let outgoingOnly = (outgoing["auth"] ?? "").lowercased().split(separator: ",").allSatisfy { $0 == "oauth2" }
                let oauthOnly = auth.lowercased().contains("oauth2") && (incomingOnly || outgoingOnly)
                let smtpUsername = (outgoing["username"] ?? "%EMAILADDRESS%").replacingOccurrences(of: "%EMAILADDRESS%", with: address).replacingOccurrences(of: "%EMAILLOCALPART%", with: String(parts[0])).replacingOccurrences(of: "%EMAILDOMAIN%", with: domain)
                let account = MailAccount(id: id, label: domain, address: address, username: username, imapHost: ih, imapPort: ip, smtpHost: sh, smtpPort: sp, smtpSecurity: outgoing["socketType"] == "SSL" ? "tls" : "starttls", smtpUsername: smtpUsername)
                return MailDiscoveryResult(account: account, explanation: oauthOnly ? L("Anbieter meldet ausschließlich OAuth. Diese Anmeldung ist noch nicht eingerichtet.") : L("Serverdaten gefunden. Bitte vor dem Verbinden prüfen. Bei aktivierter 2-Faktor-Anmeldung kann ein App-Passwort nötig sein."), oauthOnly: oauthOnly, source: response.url?.host ?? endpoint)
            } catch { continue }
        }
        throw YOBROError.message(L("Keine sichere automatische Konfiguration gefunden. Wähle deinen Anbieter oder trage die IMAP-/SMTP-Daten manuell ein."))
    }
}

final class ProviderXML: NSObject, XMLParserDelegate {
    var servers: [[String: String]] = []
    var current: [String: String]?
    var text = ""
    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
        text = ""
        if ["incomingServer", "outgoingServer"].contains(elementName) { current = ["type": attributeDict["type"] ?? ""] }
    }
    func parser(_ parser: XMLParser, foundCharacters string: String) { text += string }
    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        guard current != nil else { return }
        if ["incomingServer", "outgoingServer"].contains(elementName) { servers.append(current!); current = nil }
        else if elementName == "authentication" {
            let previous = current?["auth"] ?? ""
            current?["auth"] = previous + text.trimmingCharacters(in: .whitespacesAndNewlines) + ","
        }
        else { current?[elementName] = text.trimmingCharacters(in: .whitespacesAndNewlines) }
    }
}
