import SwiftUI
import Security
import AuthenticationServices

@MainActor
final class PasskeyAccess: ObservableObject {
    @Published private(set) var message = ""
    @Published private(set) var available = false
    @Published private(set) var requesting = false
    private let manager = ASAuthorizationWebBrowserPublicKeyCredentialManager()
    init() { refresh() }
    func refresh() {
        var code: SecCode?
        var info: CFDictionary?
        var staticCode: SecStaticCode?
        guard SecCodeCopySelf([], &code) == errSecSuccess, let code,
              SecCodeCopyStaticCode(code, [], &staticCode) == errSecSuccess, let staticCode,
              SecCodeCopySigningInformation(staticCode, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let values = info as? [String: Any],
              let entitlements = values[kSecCodeInfoEntitlementsDict as String] as? [String: Any],
              entitlements["com.apple.developer.web-browser.public-key-credential"] as? Bool == true else {
            available = false
            message = L("Passkeys sind in diesem lokalen Build noch nicht verfügbar. Dafür benötigt YoBro Apples Browser-Freigabe und eine entsprechend signierte App. Bei Google vorerst eine andere Anmeldemethode wählen.")
            return
        }
        available = true
        switch manager.authorizationStateForPlatformCredentials {
        case .authorized: message = L("Passkeys sind für YoBro freigegeben. Websites können die macOS-Anmeldung anfordern.")
        case .denied: message = L("Passkey-Zugriff wurde abgelehnt. Bitte die Freigabe für YoBro in den macOS-Einstellungen prüfen.")
        default: message = L("Erlaube YoBro den Zugriff auf Passkeys, um dich auf Websites anzumelden.")
        }
    }
    func authorize() async {
        guard available, !requesting else { return }
        requesting = true; defer { requesting = false }
        _ = await manager.requestAuthorizationForPublicKeyCredentials()
        refresh()
    }
}

struct PasskeySettings: View {
    @StateObject private var access = PasskeyAccess()
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Passkeys", systemImage: "person.badge.key.fill").font(.headline)
            Text(access.message).font(.system(size: 12)).foregroundStyle(.secondary)
            if access.available { Button(L("Passkey-Zugriff freigeben", "Allow passkey access")) { Task { await access.authorize() } }.disabled(access.requesting) }
        }.frame(maxWidth: .infinity, alignment: .leading).yobroCard(padding: 14, emphasized: true)
    }
}
