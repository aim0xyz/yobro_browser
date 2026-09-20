# YoBro für iPhone und iPad

Native SwiftUI/WebKit-App ab iOS 17, mit vorhandener YoBro/Supabase-Anmeldung. Das Xcode-Projekt und `project.yml` enthalten das eingerichtete Development Team. XcodeGen erhält diese Signierung beim Neugenerieren.

## Starten

`ios/YoBroMobile.xcodeproj` öffnen, Scheme **YoBroMobile**, iPhone oder Simulator auswählen, **Run**. Ohne bestehende Anmeldung öffnet sich sofort der lokale Browser. Ein Konto ist optional und kann unter Einstellungen → Konto → Optional anmelden verbunden werden. Tokens liegen im iOS-Schlüsselbund; Registrierung und Passwort-Reset sind ebenfalls enthalten. E-Mail-Links verwenden die vorhandene YoBro-Website und anschließend `yobro://auth`.

Für eine Geräteinstallation muss das iPhone entsperrt, gekoppelt und im Entwicklermodus sein. Personal VPN ist im Target aktiviert. Es gibt noch keine TestFlight-/App-Store-Veröffentlichung und keine Freigabe als iOS-Standardbrowser.

## Gestaltung und Nutzung ohne Konto

Die iOS-App kompiliert dieselbe `Sources/YOBRO/Theme.swift` wie die Mac-App. Seiten, Bedienelemente, Eingabefelder, Text und Akzentfarben wechseln automatisch mit dem System zwischen den gemeinsamen Light-/Dark-Paletten. Die Originaldatei `YOBROMark.png` wird direkt eingebunden; das iOS-App-Symbol verweist auf das vorhandene Desktop-Master-Asset.

Ohne Konto sind Tabs, Notizen, Lesezeichen, Verlauf, Downloads, uBlock Lite, dunkle Webseiten und die VPN-Einstellungen verfügbar. Das lokale Profil hat eine stabile, geräteinterne Identität und wird bei Neustarts wieder geöffnet. Kontoanmeldung wechselt in das separate Kontoprofil; Abmeldung kehrt zu den unveränderten lokalen Daten zurück. Kein automatisches Zusammenführen oder Hochladen des Gastprofils. Nur Desktop-Sync benötigt ein Konto.

## Browser

- Tabs mit Wiederherstellung, Schließen und Wiederöffnen, Zurück/Vorwärts, Neuladen, Suche, Teilen, Seitensuche und Ladefortschritt.
- Lesezeichen, lokaler Verlauf (maximal 500 Einträge), Notizen mit Seitenbezug.
- Pro YoBro-Konto eigene WebKit-Sitzungen, Downloads und lokale Daten. Abmeldung behält lokale Daten für dieses Konto, beendet Medien und laufende Downloads; ein systemweites VPN bleibt bestehen.
- JavaScript-Dialoge zeigen den Website-Host, können abgebrochen werden und werden beim Tabwechsel geschlossen. Popup-Fenster übernehmen die von WebKit gelieferte Konfiguration.
- Downloads über WebKit erhalten die Website-Sitzung, speichern ohne Namenskollision und lassen sich über Teilen in Dateien sichern. Abbrechen wird unterstützt; kein Fortsetzen abgebrochener Downloads.
- Webseiten dürfen HTTP verwenden; Konto-/Sync-Anfragen nutzen weiterhin HTTPS. Externe App-URL-Schemes werden nicht automatisch geöffnet.

## uBlock Origin Lite und dunkle Webseiten

Ab iOS 18.6 wird die vorhandene, mitgelieferte Safari-Version von **uBlock Origin Lite** geladen. Der Start prüft Hintergrunddienst, Filterinitialisierung und installierte Regeln im Hintergrund. Die native Startseite ist sofort verfügbar; Webseiten starten nach Vorbereitung des Basisfilters. Bis uBlock bereit ist, bleibt der Basisfilter aktiv. Die erste Erweiterungsinitialisierung kann länger dauern. Popup im Browser-Menü, Filterlisten und Optionen unter Einstellungen → Webseiten. Einstellungen sind pro Konto getrennt.

Das Paket ist uBO **Lite**, nicht die klassische Firefox-/Desktop-Erweiterung. Version, Upstream-Quelle, reproduzierbare Anpassung und Lizenz stehen in `Sources/YOBRO/Resources/uBlockOriginLite-NOTICE.txt`; Lizenz und README liegen im mitgelieferten Archiv. Neue Paketversionen benötigen derzeit ein App-Update.

Auf iOS 17–18.5 oder bei einem uBlock-Startfehler greift ein deutlich gekennzeichneter nativer Basisfilter mit zehn Drittanbieter-Domains. Der Basisfilter ist kein gleichwertiger Ersatz. Wenn auch dessen Kompilierung fehlschlägt, ist kein Filter verfügbar.

Dark Reader wird in einer isolierten JavaScript-Welt ausgeführt. Die dunkle Darstellung ist global abschaltbar, außerdem über das Browser-Menü pro Website. Website-Einstellungsänderungen laden offene Seiten neu.

## Desktop-Abgleich

Einstellungen → Konto → Desktop-Sync. Zuerst Desktop-Sync am Mac aktivieren; dann das Cloud-Profil auswählen und einmal den Wiederherstellungscode vom Mac eingeben. Der Code wird erst nach erfolgreicher Entschlüsselung gespeichert. Die Kontoanmeldung allein ersetzt diesen zufälligen Verschlüsselungsschlüssel nicht.

- **Vom Desktop übernehmen:** neue Desktop-Tabs, Notizen als Text und Lesezeichen importieren.
- **Neue Einträge auch zum Desktop senden:** neue mobile Tabs/Notizen in den Desktop-Space „Mobile“ und neue Lesezeichen in dessen Ordner ergänzen.
- Wiederholtes Übernehmen erzeugt keine Duplikate. Vor dem Schreiben wird der aktuelle Cloud-Stand gelesen; die Server-Revision verhindert das Überschreiben gleichzeitiger Änderungen.
- Bestehende Desktop-Einträge und unbekannte Felder bleiben erhalten. Das ist ein manueller, ergänzender Abgleich: spätere Bearbeitungen und Löschungen bereits abgeglichener Einträge werden noch nicht übertragen. Kein automatischer Hintergrund-Sync, keine Synchronisation von Cookies, Website-Passwörtern, Verlauf oder VPN-Zugangsdaten.

Die Verschlüsselung entspricht dem Desktopformat (ChaChaPoly, 32-Byte-Schlüssel, Formatversion 1). Falsche Schlüssel und abweichende Profil-IDs werden abgelehnt.

## VPN

IKEv2/EAP über Apples `NEVPNManager`, mit eigener Serveradresse, Remote-ID und VPN-Zugangsdaten. Nach der ersten Einrichtung kann mit dem gespeicherten Schlüsselbund-Passwort erneut verbunden werden. iOS fragt vor dem Speichern der Konfiguration nach Erlaubnis. Der Status stammt von der Systemverbindung.

YoBro enthält keinen VPN-Dienst und keinen VPN-Server. Ein kompatibler Anbieter ist erforderlich. Alternativ funktioniert eine vorhandene iOS-VPN-App systemweit. Kein Kill Switch; kein Schutzversprechen während des Verbindungsaufbaus. Ein echter VPN-Tunnel muss auf einem physischen Gerät mit Anbieterzugang getestet werden, nicht im Simulator.

## Entwicklung und Prüfung

```sh
xcodegen generate --spec ios/project.yml
xcodebuild -project ios/YoBroMobile.xcodeproj -scheme YoBroMobile \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  -derivedDataPath /tmp/yobro-mobile-build CODE_SIGNING_ALLOWED=NO test
xcodebuild -project ios/YoBroMobile.xcodeproj -scheme YoBroMobile \
  -destination 'generic/platform=iOS' -derivedDataPath /tmp/yobro-mobile-signed build
```

Tests decken Adressverarbeitung, Zustandswiederherstellung, native Filterkompilierung, den tatsächlichen uBlock-Start, Verschlüsselung und Schlüsselprüfung, Erhalt vorhandener Desktop-Daten, wiederholte Imports, Dateinamen und Dialog-Abschluss ab.

Geräteinstallation wurde am 17.09.2026 versucht; iOS blockierte sie wegen des gesperrten iPhones. YoBro wurde stattdessen im iPhone-17-Simulator gestartet. Echte Kontoabläufe, vollständige Website-Anmeldeflüsse, Cloud-Zugriff und VPN benötigen weiterhin Abnahme mit den jeweiligen Zugangsdaten. Die native Anmeldeseite wurde visuell geprüft.

Referenzen: [WebKit-Erweiterungen](https://webkit.org/blog/16574/webkit-features-in-safari-18-4/), [Personal VPN](https://developer.apple.com/documentation/networkextension/nevpnmanager).

### Prüfergebnis dieser Erweiterung

Simulator-Build und signierter iPhone-Build sind erfolgreich. Elf Tests für Kernfunktionen bestanden; der anfangs fehlgeschlagene uBlock-Test bestand nach Anpassung der asynchronen Initialisierung separat und bestätigte vorhandene Filterregeln. Ein anschließender vollständiger Wiederholungslauf blieb trotz Neustart im Xcode-Testdienst hängen, bevor Tests ausgeführt wurden. Deshalb wird kein vollständig grüner abschließender Gesamtlauf behauptet. Die laufende Nutzer-App im iPhone-17-Simulator wurde davon getrennt gehalten.

### Gemeinsames Design und lokales Profil (17.09.2026)

Drei gezielte XCTest-Tests bestanden: Wiederherstellung lokaler Tabs/Notizen ohne Konto, Ausschluss von Cloud-Zugangsdaten im Gastmodus und korrekte Auflösung der gemeinsamen Palette in Light/Dark. Der macOS-Zweig der gemeinsamen Theme-Datei wurde zusätzlich typgeprüft. Die native Startseite wurde im iPhone-17-Simulator in beiden Darstellungen geprüft.
