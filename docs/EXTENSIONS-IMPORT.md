# Erweiterungen und Browserimport

Öffnen über **Erweiterungen & Import** in der Seitenleiste, das Puzzle-Symbol in der Toolbar oder **⌘,**.

## Erweiterungen

macOS 15.4+ bietet die native WebKit-WebExtension-Laufzeit. Auf macOS 14 bleiben Browser und Datenimport nutzbar, die Installation ist deaktiviert.

- Ein lokaler Ordner mit `manifest.json`, ZIP oder CRX kann hinzugefügt werden. CRX 2/3 wird in das enthaltene ZIP umgewandelt; eine Herausgebersignatur wird nicht verifiziert. Installation nur aus vertrauenswürdiger Quelle.
- YOBRO kopiert das Paket in sein eigenes Datenverzeichnis. Name, Version, angeforderte APIs, Seitenzugriff und WebKit-Parserhinweise werden vor der Installation angezeigt.
- Erst der Installationsbutton lädt die Erweiterung und gewährt die angezeigten Rechte. Optionale Rechte werden bei Bedarf separat erfragt und gelten für die laufende Sitzung.
- Aktivieren, Deaktivieren, Entfernen, Toolbar-Aktionen/Popups und Optionsseiten sind integriert. Bereits offene Seiten bei Bedarf neu laden; bereits ausgeführte Seitenänderungen lassen sich durch Deaktivieren nicht rückgängig machen.
- Content Scripts, Nachrichten, Hintergrund-Service-Worker, Speicher, Tab-Abfragen und interne Seiten über `chrome.tabs.create()` wurden mit einer synthetischen Erweiterung geprüft. Interne Erweiterungsseiten verwenden die von WebKit vorgeschriebene kontextspezifische WebView-Konfiguration. Das ist keine Kompatibilitätszusage für beliebige Store-Erweiterungen.
- Chrome-Web-Store-Seiten öffnen: Der Toolbar-Button **In YOBRO installieren** lädt das öffentliche Paket in eine Berechtigungsvorschau. Alternativ den Store-Link unter Erweiterungen einfügen. Nicht verfügbare oder inkompatible Pakete zeigen einen Fehler. Keine automatischen Erweiterungsupdates. Keine native Messaging-Anbindung und kein Erweiterungszugriff auf YOBROs Lesezeichen-, Verlauf-, Download- oder Sitzungsverwaltung. Weitere Unterschiede gegenüber Chromium hängen von WebKit ab.
- Entfernen löscht die Registrierung und Paketkopie. Von WebKit verwaltete Speicherdaten können bis zur Bereinigung des Browserdatenspeichers verbleiben; eine erneute Installation erhält eine neue ID.

## Importauswahl

Nach Auswahl von Browser und Profil erkennt YOBRO automatisch alle unterstützten Datenarten und zeigt deren Verfügbarkeit. Passwortkonten werden zunächst nur erkannt. „Passwörter entsperren“ liest die Zugangsdaten nach der Systemfreigabe in die Vorschau; Firefox fragt gegebenenfalls nach dem Hauptpasswort. Verschlüsselte Cookies benötigen weiterhin einen Export. Jede Datenart ist einzeln wählbar. Ein Profilwechsel leert die Vorschau einschließlich entschlüsselter Passwörter und ergänzter Dateien. „Erneut erkennen“ aktualisiert die Profildaten und behält ergänzte Dateien bei. Die Bestätigung nennt die Anzahl je ausgewählter Datenart. Erst danach schreibt YOBRO in seine eigenen Speicher.

| Browser | Lesezeichen | Verlauf | Offene Tabs | Passwörter | Cookies |
|---|---|---|---|---|---|
| Safari | `Bookmarks.plist` oder HTML | `History.db` | `LastSession.plist`, sofern vorhanden | Zugängliche lokale Web-Passwörter; iCloud per Export aus Apples Passwörter-App | Netscape-TXT/JSON-Export |
| Chrome | `Bookmarks` oder HTML | `History` | unverschlüsselte `Sessions/Session_*` (SNSS 1/3) | Direkt über macOS-Schlüsselbund (v10) oder CSV | Netscape-TXT/JSON-Export |
| Brave | wie Chrome | wie Chrome | wie Chrome | Direkt über macOS-Schlüsselbund (v10) oder CSV | Netscape-TXT/JSON-Export |
| Arc | Chromium-Lesezeichen oder HTML | wie Chrome | `StorableSidebar.json` mit Spaces, Sidebar-Ordnern, angepinnten und offenen Tabs; Chromium-Sitzung nur als Fallback | Direkt über macOS-Schlüsselbund (v10) oder CSV | Netscape-TXT/JSON-Export |
| Firefox | `places.sqlite` oder HTML | `places.sqlite` | Firefox JSONLZ4-Sitzung | Direkt über installiertes Firefox/NSS, ggf. Hauptpasswort; alternativ CSV | `cookies.sqlite` ohne Container-Cookies oder TXT/JSON |

**Grenzen:** Der Quellbrowser kann Dateien sperren; Safari kann durch macOS-Datenschutz geschützt sein. Dann den Browser schließen, den Profilordner bewusst auswählen oder einen Export benutzen. Neuere verschlüsselte Chromium-Sitzungen (z. B. SNSS 5) werden nicht entschlüsselt. Safari-Profile außerhalb des ausgewählten Ordners, allgemeine Tabgruppen, Erweiterungseinstellungen, Autofill-Adressen, Kreditkarten, Passkeys und Website-LocalStorage werden nicht migriert. Verschachtelte Arc-Ordner werden wegen YOBROs flachem Ordnermodell als Pfadnamen wie `Projekt / Recherche` angelegt. Ein Cookie-Import garantiert keine aktive Anmeldung.

Zusätzlich unterstützte Dateien:

- Lesezeichen: Netscape-HTML, Chromium-Bookmarks-JSON oder Safari-Plist.
- Verlauf: JSON-Liste `{url,title,timestamp,visits}` mit Unix-Sekunden, alternativ `{history:[...]}` oder Chrome-Takeout `Browser History` mit `time_usec`.
- Tabs: UTF-8-Text mit einer HTTP(S)-URL pro Zeile, JSON-Liste aus URLs oder `{url,title,pinned}`, `{tabs:[...]}` oder Firefox-Sitzungsdatei.
- Passwörter: CSV mit `url`/`URL`/`website`, `username`/`Username` und `password`/`Password`. Mehrzeilige und zitierte Werte werden berücksichtigt.
- Cookies: Netscape-TXT oder JSON-Liste mit `name,value,domain,path,expirationDate,secure,httpOnly,sameSite`, alternativ `{cookies:[...]}`. Partitionierte JSON-Cookies werden übersprungen. Netscape-TXT transportiert kein SameSite-Attribut.

## Speicherung und bestehende Daten

Lesezeichen bleiben einschließlich Ordnerpfaden in einer separaten Bibliothek. Verlauf wird nach URL zusammengeführt, Besuchszahlen werden nicht bei jedem Import addiert. Es gibt keine automatische Kürzung des importierten Verlaufs beim nächsten Seitenbesuch. Tabs mit bereits geöffneten URLs werden übersprungen; neue Tabs laden erst bei Auswahl, auch nach einem Neustart.

Passwörter werden im **macOS-Schlüsselbund** gespeichert, nicht in YOBRO-JSON-Dateien. Unter „Passwörter“ können Konten geladen und Benutzername/Passwort kopiert werden. Die unveränderte Zwischenablage wird nach 30 Sekunden geleert. Automatisches Ausfüllen und Passwort-Synchronisation sind nicht implementiert. CSV-Exporte enthalten Klartext und sollten nach erfolgreicher Übernahme bewusst entfernt werden.

Vorhandene Passwörter und Cookies werden standardmäßig übersprungen. Ersetzen ist jeweils eine eigene Option. Abgelaufene/ungültige Cookies werden übersprungen. Importierte Geheimnisse werden nur im Speicher und über den lokalen Prozess-Pipe transportiert, nicht in Logs oder temporären Dateien.

Der Import nutzt Python 3 ohne zusätzliche Pakete, wie bereits das Mailmodul. SQLite-Dateien werden read-only geöffnet, inklusive verfügbarer WAL-Daten. Die Testprofile benutzen einen flüchtigen WebKit-Datenspeicher.

## Prüfung

```sh
python3 -m unittest discover -s tests -p test_browser_import.py
swift test
./scripts/build.sh
```

Quellen zur Plattform und den Formaten:

- [WebKit: Browser Web Extension APIs](https://webkit.org/blog/16574/webkit-features-in-safari-18-4/)
- [Apple: WKWebExtension](https://developer.apple.com/documentation/webkit/wkwebextension)
- [Mozilla: Profile und Dateien](https://support.mozilla.org/en-US/kb/profiles-where-firefox-stores-user-data)
- [Chromium: Session-Kommandos](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/sessions/core/session_service_commands.cc)
- [Chromium: Sitzungsdateiversionen](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/sessions/core/command_storage_backend.cc)

## NordVPN und Passkeys

Das geprüfte NordVPN-Chrome-Paket 6.0.1 fordert unter anderem `proxy`, `privacy`, `webRequestAuthProvider` und `offscreen` an. Die Chrome-Proxy-API ist in YOBRO nicht implementiert. YOBRO weist Pakete mit dieser zwingenden Berechtigung mit einer konkreten Erklärung zurück; eine erfolgreiche Paketinstallation würde hier keinen VPN-Schutz herstellen. Andere von WebKit ausgefilterte Manifest-Berechtigungen werden zusätzlich in der Vorschau genannt.

Passkeys auf beliebigen Websites benötigen Apples Freigabe für Browser und eine gültig provisionierte/signierte Anwendung mit `com.apple.developer.web-browser.public-key-credential`. Der lokale Ad-hoc-Build besitzt sie nicht. Unter **Passwörter → Passkeys** zeigt YOBRO diesen Status; in einem entsprechend berechtigten Build lässt sich dort die Systemfreigabe anfordern. Eine lediglich hinzugefügte Entitlement-Datei ersetzt Apples Genehmigung nicht. Der Google-Passkey-Login bleibt im lokalen Build daher offen.

Apple: https://developer.apple.com/documentation/authenticationservices/passkey-use-in-web-browsers
Apple entitlement: https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.web-browser.public-key-credential

## Gesperrte Browserdatenbanken

Bei SQLite-Sperren versucht YOBRO eine private temporäre Kopie der Datenbank einschließlich WAL. Dateistände vor/nach dem Kopieren müssen übereinstimmen; aktive Rollback-Journale werden abgelehnt, gelöschte Journal-Header werden berücksichtigt. Die Kopie wird mit SQLite `quick_check` geprüft. Veränderte Dateistände werden bis zu dreimal versucht; danach lautet die Aktion „Quellbrowser beenden und Erneut erkennen“. Das Original wird nicht geschrieben. Kopien sind auf 512 MB begrenzt, liegen in einem Verzeichnis mit Modus 0700 und werden nach Lesen/Fehler entfernt. Ein Prozessabsturz kann temporäre Dateien bis zur Systembereinigung zurücklassen.

Eine bloß gesperrte Passwortdatenbank verhindert nicht länger den Hinweis zum CSV-Export. Die Passworterkennung entschlüsselt keine Zugangsdaten. Verschlüsselte Cookies bleiben davon unabhängig eingeschränkt.
