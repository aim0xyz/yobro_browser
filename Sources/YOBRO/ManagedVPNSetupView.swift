import SwiftUI

struct ManagedVPNSetupView: View {
    @ObservedObject var vpn: ManagedVPNStore
    @State private var name = ""
    @State private var country = ""
    @State private var ip = ""
    @State private var accessKey = ""
    @State private var message = ""
    var body: some View {
        DisclosureGroup(L("Eigenen Outline-Zugang einrichten", "Set up your own Outline access")) {
            VStack(alignment: .leading, spacing: 10) {
                TextField(L("Standortname", "Location name"), text: $name)
                TextField(L("Ländercode, z. B. DE", "Country code, e.g. DE"), text: $country)
                TextField(L("Erwartete öffentliche Ausgangs-IP", "Expected public exit IP"), text: $ip)
                SecureField(L("Outline-Zugangsschlüssel (ss://…)", "Outline access key (ss://…)"), text: $accessKey)
                Text(L("Den Schlüssel erhältst du von deinem VPN-Anbieter. Er wird nur im macOS-Schlüsselbund gespeichert. Speichern stellt noch keine Verbindung her.", "Get the key from your VPN provider. It is stored only in macOS Keychain. Saving does not connect yet."))
                    .font(.caption).foregroundStyle(.secondary)
                Button(L("Auf diesem Mac speichern", "Save on this Mac")) {
                    do {
                        try vpn.provision(city: name, countryCode: country, expectedExitIP: ip, accessKey: accessKey)
                        accessKey = ""; name = ""; country = ""; ip = ""
                        message = L("Gespeichert. Wähle den Standort zum Verbinden.", "Saved. Select the location to connect.")
                    } catch { message = error.localizedDescription }
                }
                if !message.isEmpty { Text(message).font(.caption) }
            }.padding(.top, 10)
        }.onDisappear { accessKey = "" }
    }
}
