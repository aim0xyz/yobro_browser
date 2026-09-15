# Passkey-Freigabe und NordVPN

## Passkeys: externer Freigabeschritt

Ein Developer-Account ist vorhanden; die Browser-Passkey-Freigabe fehlt noch. Sie kann nicht durch eine lokale Signatur oder einen Eintrag in einer Entitlement-Datei ersetzt werden.

Der Account Holder eines Apple-Developer-Organisationsaccounts beantragt die Berechtigung über:
https://developer.apple.com/contact/request/macos-browsers-passkeys/

Referenz:
https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.web-browser.public-key-credential
https://developer.apple.com/documentation/authenticationservices/passkey-use-in-web-browsers

Nach Genehmigung ein Provisioning-Profil für die exakte YOBRO-App-ID mit der verwalteten Berechtigung erstellen und die passende Signieridentität lokal installieren. Das aktuelle Bundle verwendet `local.yobro.browser`; wenn eine andere registrierte ID verwendet wird, muss `Resources/Info.plist` passend geändert werden.

Vorbereiteter Build-Aufruf (Platzhalter durch eigene Werte ersetzen):

```sh
YOBRO_SIGN_IDENTITY='Developer ID Application: …' \
YOBRO_PROVISIONING_PROFILE='/absoluter/Pfad/YOBRO.provisionprofile' \
./scripts/build.sh
```

Der Signierschritt prüft Ablaufdatum, exakte App-ID und Passkey-Berechtigung, bettet das Profil ein und signiert mit Hardened Runtime. Ohne diese Variablen bleibt der lokale Ad-hoc-Build erhalten. Eine App-Notarisierung und ein echter Google-Passkey-Login sind anschließend gesondert zu prüfen; sie wurden noch nicht durchgeführt. Unter Einstellungen → Passwörter → Passkeys lässt sich in einem berechtigten Build der Benutzerzugriff anfordern.

## NordVPN: noch keine funktionierende Erweiterung

Untersucht wurde das öffentliche Chrome-Paket 6.0.1, ohne es zu aktivieren. Sein Hintergrund-Service-Worker verwendet Proxy-Konfiguration im Modus `pac_script`, `onAuthRequired` sowie Offscreen-Funktionen. Das Manifest fordert zusätzlich `privacy` und `webRequestAuthProvider` an. WebKit filtert mehrere dieser Berechtigungen beim Parsen heraus, ohne einen ausreichenden Fehler zu melden.

WebKit bietet native Proxy-Konfiguration für WKWebsiteDataStore, aber diese ist keine Implementierung der Chrome-Extension-API:
https://developer.apple.com/documentation/webkit/wkwebsitedatastore/proxyconfigurations-6g21z

Für die konkrete NordVPN-Erweiterung fehlen mindestens eine Extension-Bridge im Hintergrund-Service-Worker, PAC-Auswertung und Anfrage-Routing, Proxy-Authentifizierung, Offscreen-Laufzeit sowie verlässliche Fehler- und Verbindungszustände. Eine einzelne feste Proxy-Einstellung würde diese Anforderungen nicht erfüllen. Die bestehende Ablehnung bleibt deshalb bestehen. Keine Behauptung von VPN-Schutz und keine Änderung der System-Netzwerkeinstellungen.

Eine Umsetzung erfordert eine eigene Chrome-Kompatibilitätsschicht oder einen Wechsel zu einer dafür geeigneten Browser-Engine. Diese Architekturänderung wurde mit diesem Onboarding-Update nicht umgesetzt. Für einen späteren Integrationstest sind außerdem ein vom Nutzer bedientes NordVPN-Konto und reale Prüfungen von Authentifizierung, Routing, Ausnahmen und Verbindungsabbruch erforderlich.

## Space-Proxy

Unabhängig von der nicht unterstützten Browser-Erweiterung kann ein Space über WebKits native Proxy-Konfiguration geroutet werden. Die ausdrücklich als Niederlande bezeichnete NordVPN-Vorlage verwendet SOCKS5 über `nl.socks.nordhold.net:1080` und erwartet die separaten Service-Zugangsdaten aus dem Nord Account. NordVPN veröffentlicht derzeit SOCKS5-Endpunkte für Niederlande, Schweden und USA, aber nicht für Norwegen. Für die vollständige Länderauswahl muss deshalb die native NordVPN-App als systemweites VPN verwendet werden.

Vor dem Speichern oder Aktivieren prüft YOBRO über genau diese Proxy-Konfiguration eine HTTPS-Verbindung. Schlägt DNS, TCP, Authentifizierung, CONNECT/SOCKS oder TLS fehl, bleibt die Konfiguration deaktiviert und die Fehlermeldung wird angezeigt. Proxy-Passwörter werden im macOS-Schlüsselbund gespeichert; `space-proxies.json` enthält nur nicht geheime Metadaten. Ein systemweit aktives VPN verwendet weiterhin die normalen macOS-Netzwerkeinstellungen und benötigt keine Space-Proxy-Konfiguration.
