# Chromium-Fortschritt

**Zuletzt aktualisiert:** 16. September 2026  
**Aktueller Stand:** Technische Engine-Grundlagen weit fortgeschritten, Produktoberfläche in früher Preview und in Migration auf die Qt-Quick-Shell (PLAN.md). Die Chromium-Version hat **noch nicht** den Funktionsumfang der WebKit-App.  
**Testbare App (aktuellster Stand):** `chromium/build/YOBRO Chromium Feasibility.app`  
**Zuletzt gepackte App:** `dist/chromium-macos-arm64/YOBRO Chromium Feasibility.app` (älter als der aktuelle Quellstand)

## Ehrliche Einordnung

| Bereich | Stand |
| --- | --- |
| Engine, Profile, Architektur | weit fortgeschritten |
| Agent-Protokoll v2 und Sicherheitsgrundlagen | solide |
| Sichtbare Browser-Grundfunktionen | teilweise vorhanden |
| WebKit-Funktionsparität | deutlich entfernt |
| Produktreife | frühe Preview |

Die bestandenen Tests belegen Engine-, Sicherheits- und Protokollverhalten. Sie belegen **nicht**, dass die sichtbare Browser-App vollständig nutzbar ist.

## Fortschritt

- [x] **1. Berechtigungen und Medienzugriff**  
  Kamera/Mikrofon, private Sitzungen, sichere Ablehnung und reale Qt-Tests sind abgeschlossen. Seit dem 15. September fragen auch Standort, Mitteilungen, Zwischenablage, Schriftenliste und Mauszeiger-Sperre nach, statt still abgelehnt zu werden; gespeichert wird keine Entscheidung.

- [x] **2. Renderer-Abstürze und Wiederherstellung**  
  User-Tabs werden kontrolliert wiederhergestellt; Agent-Tabs bleiben fail-closed. Crash- und Prozess-Tests sind abgeschlossen.

- [ ] **3. Downloads — Abschlussprüfung läuft**  
  Download-Bibliothek, User-/Private-/Agent-Downloads, Abbruch, Fehler, Neustart, Kollisionen, Cleanup und Shutdown sind implementiert. Die Gesamtsuite besteht **41/41 Tests** (Stand 16. September 2026, siehe die Bestandsaufnahme unter Meilenstein 4), inklusive eines echten Downloads. Offen sind der abschließende unabhängige Review und die Gate-Dokumentation. Die Lifetime-Warnung `Release of profile requested but WebEnginePage still not deleted` ist geklärt und behoben: sie war kein Testartefakt, sondern ein echter Besitzfehler. `QtBrowserLibrary` hält für einen ausdrücklichen Agenten-Download eigene `QWebEnginePage`-Objekte und behielt bis zu acht davon zurück, um eine spät eintreffende Download-Anfrage noch zuordnen zu können — die Anwendung gibt aber unmittelbar nach `shutdownDownloads()` die Sitzung und damit das Profil frei, also überlebten diese Seiten ihr Profil. `shutdownDownloads()` gibt sie jetzt alle frei (nach dem Herunterfahren wird eine späte Anfrage ohnehin abgelehnt), das neue `retainedNativePages()` macht das prüfbar, und `chromium-download-lifecycle` fängt die Qt-Warnung jetzt über einen eigenen Message-Handler ab und schlägt fehl, wenn sie wieder auftritt — eine Warnung, an der niemand scheitert, behebt niemand.

- [ ] **4. Sichtbare WebKit-Produktoberfläche — in Arbeit**  
  **Aktueller Schwerpunkt.** Sidebar, Tabs, Toolbar, Bibliothek, Einstellungen, Onboarding und weitere WebKit-Oberflächen werden auf Chromium übertragen.

  Bereits übertragen (Teilschritt „QML-Fundament & erste Lieferung“, 16. September 2026):
  - Die Migration auf die Qt-Quick-Shell hat begonnen (PLAN.md). Neu ist das separate App-Ziel `yobro-chromium-qml` (Bauschalter `YOBRO_BUILD_QML_SHELL`, standardmäßig an) — das Widget-Fenster bleibt unverändert die funktionale Referenz, bis die QML-Ansicht die jeweilige Funktion übernommen hat. Die QML-App ist widgets-frei (`QGuiApplication`, `QQmlApplicationEngine`, `QtWebEngineQuick`).
  - Die Brücke hält die Trennung aus dem Plan ein: `QmlTheme` liest jede Farbe live aus der bestehenden Theme-Schicht (`themePalette`), es gibt keine zweite Token-Tabelle — der `chromium-theme-tokens`-Test bewacht weiter die einzige Quelle. `QmlIconProvider` rendert dieselben eingebetteten Lucide-SVGs wie das Widget-Fenster, tintbar über `image://yobroicon/<name>?role=…&dark=…`; der Glow der Startseite kommt aus `QmlGlowProvider` als exaktes `RadialGradient(moss 8 %, r 20→420)`-Textur in Fensterauflösung. `QmlBrowserModel` ist die erste Scheibe des `BrowserUiModel`: eine URL, Titel, Ladezustand, Zurück/Vor und die wenigen Navigationsaktionen — Aktionen fließen ausschließlich C++ → `navigationRequested` → View, Seitenzustand meldet der View über `reportPageState` zurück. QML trifft keine Navigationsentscheidung.
  - Die erste Lieferung aus dem Plan ist vollständig: QML-Fenster mit echtem `WebEngineView` auf einem realen Profil (derselbe `ProfilePaths`-Speicher wie das Widget-Fenster, Cookie-Richtlinie aus `PrivacySettings`, Startup-Proxy-Fail-closed wie im Widget-Startpfad), statische Sidebar (Markenzeile, Adressfeld, Mail, Space-Zeile, Tabliste mit Aktionen, Bibliotheksleiste, Agentenkarte, Profil-Fußzeile), Papier-Content-Karte (Radius 14, Haarlinie, 10 pt Einrückung) und echte Adressnavigation mit einem Tab. Tippfeld-Normalisierung identisch zum Widget-Fenster (Wörter ohne Schema → DuckDuckGo, sonst `fromUserInput`, nur http(s)).
  - Sichtbare Abnahme automatisiert: `--capture <png>` zeigt die Shell im Referenzfensterformat (1360×808), zwei goldene Referenzen aus der SwiftUI-App liegen unter `chromium/tests/golden/`, und der Test `chromium-qml-shell-capture` erzeugt Hell- und Dunkel-Aufnahmen plus Differenzbild mit Zahlenwert (`VisualReferenceDiff.py`, Masken für die Traffic Lights, Warnschwelle 12/255, Fail-Schwelle 45/255). `chromium-qml-shell-navigation` fährt eine file://-Seite durch das Modell und prüft pixelgenau, dass der Webinhalt statt der Startseite erscheint.
  - Zwei erwähnenswerte Qt-Fallen, die die Tests aufgedeckt haben: Q_ENUM-Werte sind über ein Kontext-Property nicht erreichbar (die Navigationsaktionen sind deshalb einfache `int`-Properties mit explizitem `READ` — ohne das Keyword löst moc den Accessor nicht auf), und ein `Layout.fillHeight`-Kind in einem verschachtelten RowLayout bläht die Zeile selbst auf. Die SwiftUI-Schriften (SF Pro Rounded/New York) sind für Nicht-Apple-Prozesse unsichtbar; die Shell nutzt deshalb die gleichen Fallback-Ketten wie das Widget-Fenster (System-Sans, Menlo) — die gerundete Schnitt läuft über die CoreText-Registrierung als spätere Verfeinerung.
  - Bekannte Grenze der goldene Referenzen: die Aufnahmen `ref-webkit-*.png` stammen aus einem älteren SwiftUI-Stand und zeigen noch keine Content-Karte und kein Profil-Kärtchen; die QML-Shell folgt dem heutigen `BrowserShell.swift` (Karte, Padding 10, Profil-Fußzeile als Karte), weshalb der Hell-Vergleich als WARN (22/255) und nicht als Fail läuft. Verbindliche neue SwiftUI-Referenzaufnahmen im heutigen Stand sind der offene Teil von Phase 1.

  **Teststand: 41/41.** Die Sidebar- und Panel-Überarbeitung hatte sieben Widget-UI-Tests rot hinterlassen und mehrere Bedienelemente funktionslos gemacht; beides ist hier geschlossen. Repariert als echte App-Fehler: der Verlauf/Lesezeichen-Knopf neben dem Adressfeld (`libraryButton`) und der Rückgängig-Pill nach dem Tab-Schließen (`reopenClosedTabButton`) hatten ihren Klick-Handler und teils ihren Objektnamen verloren und sind wieder verdrahtet; die Bibliotheksleiste hat ihren Downloads-Knopf zurück (`downloadsButton` → `showDownloads()`, WebKit-leistengetreu statt eines doppelten Neuer-Tab-Plus); der Senden-Knopf des Assistenten blieb beim Tippen dauerhaft deaktiviert, weil sein Zustand nur beim Panel-Refresh und nicht bei Textänderung gepflegt wurde (`updateSendState` läuft jetzt auf jedes Textänderung); die Assistent-Timeline trug ihren Text nur noch in den Bubble-Widgets und war damit für Screenreader und Tests unlesbar (Item-Text wieder gesetzt); die Agentenkarte zeigt wieder ihren Zustand an (offen/zu, wie die WebKit-Karte); die versteckten Space-/Ordner-Picker haben ihre Objektnamen zurück. In den Tests wurden die neuen Elementnamen übernommen, der flache Sidebar-Baum (Gruppen auf Top-Level statt Space-Zeile) in den Helfern nachgezogen, der Assistent-Fluss ereignisschleifen-tolerant geprüft, der Import-Einstieg über den Menüpunkt (`importMenuAction`) gefahren und nach dem Session-Restore der Wiederöffnen-Zustand aktualisiert. Alle 41 Tests bestehen (Stand 16. September 2026).

  Bereits übertragen (Teilschritt „Design-Fundament & Shell-Exaktheit“, 16. September 2026):
  - Design-Tokens: `Theme.cpp` trägt jetzt die exakte WebKit-Palette aus `Sources/YOBRO/Theme.swift` (ink, moss, paper, surface, chrome-Gradient, sage/lilac/peach, alle sieben Ordnerfarben, field, border, brandOrange) in Hell und Dunkel, dazu die Deckkraft-Mischungen, die die WebKit-Views mit `.opacity()` bauen. Der Test `chromium-theme-tokens` liest beide Dateien und schlägt fehl, sobald sie auseinanderlaufen; dasselbe verbietet Hex-Farbliterale außerhalb der Theme-Schicht — 18 Tokens im Gleichzug, die Spike-Dateien sind hex-frei.
  - Vektor-Icons (SF-Symbol-Ersatz): 59 tintbare Lucide-SVG-Icons unter `chromium/spike/icons/` (ISC-Lizenz) eingebettet. `installIcon()` und `retintIcons()` rendern Icons dynamisch passend zum aktiven Token (`ink`, `textMuted`, `moss`, `brandOrange`) bei 2x Retina-Auflösung mit automatischem Re-Tinting bei Hell/Dunkel-Wechsel.
  - Action-Zeilen: Sidebar-Aktionen, Bibliotheksleiste, Workspace-Tools, Suchleiste und Dialog-Buttons auf schlanke Icon-Buttons umgestellt; Textüberläufe bei langen Bezeichnungen sind beseitigt.
  - Startseite (New Tab Page): Vollständig token-adaptiv (`diagnosticStartPage`) mit radialem Glow-Gradient (`moss` 8% auf `paper`), YoBro-Mark, SF Pro Rounded Typografie, Such-Card (`surface` 72% mit `borderSoft` Stroke), Fokusring (`moss`), Tastatur-Hinweis und responsiven Discovery-Kacheln (Wikipedia, GitHub, Hacker News) sowie separatem Privatem Browsen Modus.
  - Fensterchrom: macOS `NSWindow`-Shim (`configureMacWindowFrame`) aktiviert `.fullSizeContentView` und transparente Titelleiste mit nativer Traffic-Light-Clearance in voller und kompakter Sidebar.
  - Shell-Struktur: die WebKit-App hat keine Toolbar über dem Inhalt — Zurück/Vor/Neu-laden und das Adressfeld sind in die Sidebar integriert, die permanente Statuszeile ist transient („showStatus“), der Arbeitsbereich liegt als Papier-Karte mit Haarlinien-Rand (Radius 14) auf dem Chrome-Gradient.
  - Der Hellmodus ist durchgängig repariert: Tree-Stylesheet, Item-Farben und Dialog-Overlays nutzen konsistent die dynamische Palette.
  - Typografie: Markenwortmarke „YoBro“ in SF Pro Rounded 600 mit −1,4 px Laufweite, editoriale Titel in New York (Georgia-Rückfall), Micro-Caps in SF Mono/Menlo.
  - Neue Ordner erhalten Moosgrün (WebKit-Standard) statt des alten Graus.
  - `YOBRO_TEST_APPEARANCE=light|dark` pinnt die Erscheinung für Referenzaufnahmen; Aufnahmen für Startseite und Chrome liegen unter `.runtime-refcap/`.

  Bereits übertragen (Teilschritt „Tabs, Navigation, Bedienung“):
  - Tastenkürzel ⌘T, ⇧⌘N, ⌘W, ⌘L, ⌘R, ⌘D, ⌘S, ⇧⌘S, ⌘K, ⌘F, ⌘G, ⇧⌘G, ⌘Y, ⇧⌘J, ⇧⌘T, ⇧⌘A, ⌘, und Escape
  - In-Page-Suche mit vor/zurück (Chromium meldet Treffer/kein Treffer, keine Trefferzahl)
  - Quick Switcher über offene Tabs, Verlauf und Websuche
  - Tab duplizieren (lädt die URL neu, kopiert keinen Formularzustand)
  - Sidebar ein-/ausblenden
  - Korrektur: ⇧⌘T war fehlerhaft auf ⌃⇧T gebunden

  Bereits übertragen (Teilschritt „Startseite, Favicons, Split View“):
  - Startseite mit funktionsfähigem Suchfeld und anklickbaren Verknüpfungen
  - Favicons in Sidebar und Tabs
  - Split View über ⇧⌘S; die Views werden nur umgehängt, nie kopiert

  Bereits übertragen (Teilschritt „Tab-Umordnung“):
  - Tabs per Drag-and-Drop in der Sidebar sortieren
  - Ziehen in Ordner oder in „Angepinnt“ übernimmt Ordner- und Pin-Zuordnung
  - Reihenfolge wird in `session.json` gespeichert; ältere Sessions bleiben lesbar

  Bereits übertragen (Teilschritt „Einstellungen und Profile“):
  - Einstellungsdialog über ⌘, und über die Profilzeile in der Sidebar
  - Profilliste, Profilwechsel und Anlegen neuer Profile
  - Agentenzugriff pausieren und Verlauf-/Download-Freigabe umschalten
  - Anzeige von Profil-, Socket- und Downloadpfad

  Bereits übertragen (Teilschritt „Logins und AutoFill“):
  - Passwortspeicher im macOS-Schlüsselbund, getrennt pro Profil
  - Vorschläge erscheinen beim Anklicken eines erkannten Login-Feldes
  - Schlüsselsymbol in der Adressleiste füllt Logins auf Anforderung
  - Speichern wird nach dem Anmelden angeboten und erfordert immer eine Bestätigung
  - Verwaltung gespeicherter Logins in den Einstellungen, inklusive Entfernen
  - Nur exakt gleiche HTTPS-Adressen, keine Subdomains und keine privaten Tabs
  - Formulare werden nie automatisch abgeschickt

  Bereits übertragen (Teilschritt „Paritätsliste Punkte 1–13“, 15. September 2026):
  - Tastenkürzel auf die WebKit-Belegung korrigiert (⌘D Lesezeichen, ⇧⌘T privater Tab, Wiederherstellen und Duplizieren ohne Kürzel) und um ⌘1–⌘9, ⌘+/⌘−/⌘0, ⌘P, ⌥⌘B, ⌘[ und ⌘] ergänzt
  - Echte Menüleiste mit Datei-, Browser- und Tabs-Menü; Einstellungen im macOS-App-Menü
  - Seitenzoom pro Host mit der WebKit-Stufenleiter, gespeichert in `page-zoom.json`
  - Drucken über den Systemdialog
  - Favicons, Tabtitel, aktiver Tab, Split-Partner, Sidebar-Zustand und Fenstergeometrie überleben einen Neustart
  - Kompakte Icon-Sidebar; ⌘S klappt ein und aus statt komplett auszublenden
  - Website-Darkmode mit dem geteilten DarkReader aus der WebKit-App, global oder pro Host, plus hell/dunkel für die App selbst
  - Vollständige Lokalisierung Deutsch und Englisch nach Systemsprache
  - Profile mit eigenem Namen und Symbol; der Wechsel passiert wie in der WebKit-App **im selben Fenster**
  - Lesezeichen umbenennen, in Ordner verschieben, filtern und sortieren
  - Browserdaten löschen mit Zeitraum, Cookie-Aufbewahrung wählbar
  - Standort, Mitteilungen, Zwischenablage, Schriftenliste und Mauszeiger-Sperre werden abgefragt statt still abgelehnt
  - Werbe- und Trackerfilter: dieselben 101 Domains und 14 Pfadmuster wie die WebKit-App, dazu Entfernen von Klick-Kennungen aus Adressen und die geteilten kosmetischen Filter
  - Zertifikatsvertrauen: für Webseiten gilt das Urteil der Engine ohne Umweg; nur für lokale Hosts (Loopback, private IPv4-Bereiche, `.local`, `.test`, `.internal`, `.home.arpa`) gibt es nach Anzeige des SHA-256-Fingerprints eine Ausnahme, die nur bis zum Beenden lebt
  - Proxy pro Space mit Passwort im Schlüsselbund; lässt sich der eingerichtete Proxy nicht verwenden, wird nichts geladen statt ungeschützt zu laden
  - Passwort-Import aus Chrome, Brave, Arc, Firefox und Safari über dasselbe Hilfsskript, das die WebKit-App nutzt; der Quellbrowser wird nur gelesen
  - Import von Lesezeichen, Verlauf, offenen Tabs und Cookies aus anderen Browsern, mit Profilsuche, Vorschau pro Datenart und Exportdateien als Rückfall
  - Erweiterungen aus dem Chrome Web Store: Link oder ID eingeben, Paket wird geladen, geprüft und entpackt, Berechtigungen werden vor der Installation gezeigt (zur Aktivierung siehe die Qt-Grenze weiter unten)
  - Notizen als eigene Tabart mit ⌘N, Formatierung über fett, kursiv, unterstrichen, Überschrift und Liste; Notizen liegen in Spaces und Ordnern wie Tabs und kommen mit Titel und Formatierung aus der Session zurück
  - Integrierter Assistent pro Space in der Agentenfläche (⌘⇧A, wie in der WebKit-App), mit Anbieterauswahl für OpenRouter, Anthropic, OpenAI und einen eigenen Endpunkt, Werkzeugen für Notizen, Tabs und das eingebaute Postfach, Freigabedialog vor jedem Öffnen einer Adresse und Verlauf pro Space
  - Eingebautes Postfach (⇧⌘M, wie in der WebKit-App) mit IMAP und SMTP über dasselbe geteilte Hilfsskript, automatischer Serversuche, Anbietervorlagen, Ordnerbaum, Lesen, Antworten, Senden, Löschen, Verschieben und Anhängen

  - Einrichtung beim ersten Start in drei Schritten wie in der WebKit-App, mit echter Datenübernahme im zweiten Schritt, `onboarding.json` als Merker und dem Eintrag „Einrichtung erneut öffnen …“ in den Einstellungen

  Noch offen in diesem Meilenstein: die finale WebKit-Optik.

  Bewusste Auslassungen mit Begründung:
  - Berechtigungsentscheidungen werden weiterhin nie gespeichert. Eine Übersicht in den Einstellungen wäre leer, weil das Profil auf „jedes Mal fragen“ steht.
  - Mehrere Fenster und eine wählbare Suchmaschine sind keine Paritätslücken: die WebKit-App hat beides selbst nicht.

- [ ] **5. Vollständige WebKit-Funktionsparität**  
  Von der Paritätsliste sind **alle 30 Punkte abgeschlossen**.

  Die VPN-Anbindung ist auf Wunsch ausgelassen und zählt nicht mehr zur Liste; die Liste hat damit 30 statt 31 Punkte.


  **Passkeys funktionieren mit Qt 6.11.2 nicht, und das ist schlimmer als es klingt.** Die ganze WebAuthn-Bedienoberfläche ist in Qt deklariert und einkompiliert, aber eine Anfrage aus einer Seite (`navigator.credentials.get()` oder `.create()` mit `publicKey`) bekommt ein Versprechen, das nie eingelöst wird: kein `webAuthUxRequested`, keine Ablehnung, und sogar das seitenseitige Zeitlimit wird ignoriert. Mit einem eigenständigen Probeprogramm über 14 Sekunden gemessen, unsichtbar und mit echtem Fenster, für beide Aufrufe. Eine Website wartet dadurch endlos, und ihr eigener Rückfall auf „nimm ein Passwort" läuft nie, weil der im Ablehnungszweig steht. Deshalb lehnt diese Anwendung eine Passkey-Anfrage sofort mit `NotAllowedError` ab (`chromium/spike/PasskeyShim.js`, nur im Hauptweltkontext, nur bei `publicKey`, Passwort-Credentials bleiben unberührt) — dann zeigt die Seite ihre andere Anmeldemethode. Die echte Bedienung ist vollständig gebaut und verdrahtet: Engine-Schnittstelle mit allen sieben Zuständen und dreizehn Fehlergründen, dieselbe Torprüfung wie beim Zertifikatsdialog (nur der aktive, sichtbare Nutzertab eines aktiven Profils, nie ein Agententab), und ein Dialog für Kontowahl, PIN-Eingabe mit Grund, Fehler, Mindestlänge und verbleibenden Versuchen, Schlüsselberührung und Fehlschlag mit Wiederholung. Sie greift, sobald `QtBrowserProfile::passkeysWork()` true wird; `chromium-passkeys` misst das Engine-Verhalten auf einem unberührten Profil und schlägt fehl, sobald Qt antwortet.

  **Synchronisierung: was mitfährt und was nicht.** Website-Sitzungen, Cookies, Passwörter, Mailkonten und Favicon-Bytes sind nicht Teil eines Schnappschusses; `chromium-sync-ui` prüft das am serialisierten Ergebnis. Verlaufsadressen werden vor dem Verschlüsseln gesäubert: Fragment weg, und eine Google-Anmeldeadresse wird auf ihren Host reduziert, weil dort Einmal-Token im Pfad stehen. Sammlungen, die nur wachsen (Verlauf, Lesezeichen, Spaces, Ordner), werden vereinigt statt zugewiesen — sonst würde ein Zeitstempelvergleich die Daten der anderen Seite löschen. Der Abgleich ist idempotent, deshalb darf das Ergebnis direkt zurückgeschrieben werden.

  **Vier bewusste Abweichungen bei der Synchronisierung.** Erstens ist ChaCha20-Poly1305 hier von Hand implementiert (`chromium/spike/ChaCha20Poly1305.cpp`), weil die WebKit-App CryptoKit benutzt, Qt keinen authentifizierten Chiffrierer anbietet und macOS dafür keine stabile C-Schnittstelle hat; `chromium-sync` prüft Blockfunktion, Stromchiffre, Poly1305 und die vollständige Konstruktion gegen die Testvektoren aus RFC 8439, und nur deshalb ist handgeschriebene Kryptografie hier vertretbar. Zweitens trägt der Umschlag `formatVersion` 2: die WebKit-App erklärt 1 und lehnt alles Höhere ab, weist diese Nutzlasten also sauber zurück, statt eine Tabliste zu missdeuten, deren Form sie nicht kennt. Drittens schließt „die andere Seite ist neuer" hier keine Tabs, sondern öffnet nur die fehlenden (höchstens 30) — ein falscher Abgleich würde sonst offene Arbeit wegwerfen. Viertens liegen Kontotoken und Sync-Schlüssel im Schlüsselbund unter einem eigenen Dienstnamen pro Profil, nicht unter dem `xyz.aimo.yobro.sync` der WebKit-App; der Wiederherstellungscode ist der Weg auf ein zweites Gerät.
  **Agent-Protokoll: der Befehlssatz ist vollständig, das Protokollbuch neu.** Jeder ausgeführte Agentenbefehl wird jetzt aufgezeichnet (`BrowserSession::agentEvents()`, neueste zuerst, höchstens 50 Einträge wie in der WebKit-App), erscheint in der Liste `agentActivityList` in der Agentenfläche und als `action`-Zeile im Assistentenverlauf des jeweiligen Space. Die Detailtexte sind dieselben wie in `BrowserModel.record`: Host der Seite, sonst Tabtitel, mit den Sonderfällen `Neuer Tab`, `Seitensuche`, `Seite gelesen`, `Datei`, dem Dateinamen beim Abbruch eines Downloads und `ref · Host` bei `click` und `fill`. Fehlgeschlagene Befehle werden nicht aufgezeichnet, `status`, `tabs`, `history`, `downloads`, `split` und `end` grundsätzlich nicht. Zusätzlich verweigern `click` und `fill` jetzt wie in der WebKit-App einen Tab, der die Startseite zeigt, statt die Brücke dort einzuspritzen, und `capture-window` existiert für die lokale Entwicklungsprüfung — nur mit `YOBRO_DEV_CAPTURE=1`, nie in der Befehlsliste von `status`, und es schreibt ausschließlich `window.png` neben das Profil. `chromium-agent-log` prüft das gegen den echten Controller, die echte Sitzung und ein echtes Fenster.
  Eine bewusste Abweichung bleibt: läuft `find` über den Agenten, öffnet die WebKit-App die Suchleiste des betroffenen Tabs. Hier ist die Suchleiste ein Fensterelement, das an die Seite des Nutzers gebunden ist; sie mit der Suche des Agenten zu füllen würde eine Leiste zeigen, die auf einer anderen Seite arbeitet. Die Suche des Agenten steht deshalb nur im Protokollbuch.
  **Login-AutoFill ohne Polling.** Die WebKit-App registriert einen `WKScriptMessageHandler`, die Seite meldet also selbst, wenn etwas passiert. Diese Anwendung hat stattdessen alle 350 Millisekunden `takeEvent()` in jeder https-Seite ausgewertet — eine wiederkehrende JavaScript-Auswertung auf der Seite, die der Nutzer gerade vor sich hat, die den Vorschlag verzögert und den Renderer für nichts weckt. Jetzt schiebt die isolierte Welt ihre Ereignisse über einen QWebChannel, der genau an diese Welt gebunden ist (`chromium/spike/LoginChannel.cpp`, ein Kanal und ein Empfänger pro Seite, damit ein Ereignis seine Seite mitbringt, ohne sie erfragen zu müssen). Der Kanalclient wird dem Skript vorangestellt statt als zweites Skript eingespritzt, weil Qt keine Ausführungsreihenfolge zwischen zwei Skripten desselben Einspritzpunkts zusagt. `takeEvent()` ist ganz verschwunden, der Timer ebenfalls. Der Empfänger verwirft alles, was nicht ein kleines, wohlgeformtes JSON-Objekt ist, und die Seitenwelt sieht weder das Skript noch den Transport. Zwei Prüfungen sind neu hinzugekommen: ein Ereignis, das vor einer Navigation in der Warteschlange lag, wird verworfen, wenn sein Origin nicht mehr zur geladenen Seite passt, und private Tabs bekommen gar keinen Kanal. `chromium-login-autofill` fährt das an einer echten https-Seite mit selbstsigniertem Zertifikat und echten Mausklicks durch, weil das Skript nur auf echte Nutzergesten reagiert.
  Eine Abweichung bleibt: die WebKit-App spritzt ihr Skript in alle Rahmen und lässt für Apples Anmeldung eine kurze, ausdrückliche Liste von Identitätshosts (`idmsa.apple.com`, `appleid.apple.com`, `account.apple.com`) innerhalb von `apple.com` zu. Hier läuft das Skript nur im obersten Rahmen, ein Loginformular in einem eingebetteten Rahmen wird also nicht erkannt. Das ist die engere Seite: es wird nie ein fremdes Formular gefüllt.
  **Passwortspeicher auch ohne Schlüsselbund.** `PasswordVault` hat jetzt zwei Rückseiten. Auf macOS bleibt es der Schlüsselbund, weil dort das Betriebssystem das Geheimnis hält und der Nutzer es mit eigenen Werkzeugen sehen und entziehen kann. Überall sonst — und auf Wunsch auch auf macOS über `YOBRO_PASSWORD_STORE=file` — liegt `passwords.vault` neben dem Profil, versiegelt mit ChaCha20-Poly1305 unter einem Schlüssel, den PBKDF2-HMAC-SHA256 mit 600 000 Runden aus einer Passphrase ableitet (`chromium/spike/KeyDerivation.cpp`, weil Qt HMAC hat, aber keine Schlüsselableitung, und CommonCrypto jede andere Plattform leer ausgehen lassen würde). Neuer Salt bei jedem Schreiben, Schreiben über eine Nebendatei mit Umbenennen, Rechte nur für den Besitzer, und eine beschädigte Datei wird gemeldet statt überschrieben. Diese Rückseite hat einen Zustand, den der Schlüsselbund nicht hat: **gesperrt**. Bis der Nutzer die Passphrase eingibt, wird nichts angeboten und nichts gespeichert — eine Datei, deren Schlüssel danebenliegt, wäre nur ein Passwortspeicher zum Anschauen. Der Dialog „Gespeicherte Logins“ zeigt dafür Zustand, Entsperren, Sperren und Passphrase ändern; das Ändern prüft die alte Passphrase gegen die Datei, selbst wenn der Speicher gerade offen ist. Nebenbei ersetzt `PasswordStoreResult` den früheren Rückgabewert `bool` von `store()`: ein einzelnes Ja/Nein konnte „war schon da und wurde aktualisiert“ nicht von „nichts geschrieben“ unterscheiden, weshalb die Bestätigung an den Nutzer bisher bei jeder Aktualisierung „gespeichert“ sagte.
  `chromium-password-vault` prüft die Ableitung gegen `hashlib.pbkdf2_hmac` von Python 3 — eine unabhängige Implementierung, und der einzige Grund, warum handgeschriebene Schlüsselableitung hier vertretbar ist — dazu Auswahl der Rückseite, gesperrtes Verhalten, Rundlauf, falsche Passphrase, Passphrasenwechsel, fünf beschädigte Dateien, dass kein Klartext in der Datei steht und dass zwei Profile sich nicht gegenseitig öffnen können. `chromium-login-autofill` fährt zusätzlich ein zweites Fenster mit der Dateirückseite: gesperrt wird nichts angeboten und nichts gespeichert, nach dem Entsperren über den Dialog landet ein bestätigter Login wirklich in der verschlüsselten Datei, und was während der Sperre abgesendet wurde, ist nicht darin.
  **`SpikeWindow` ist aufgeteilt.** Die Klasse war eine Übersetzungseinheit mit 6237 Zeilen; jede Änderung war eine Suche durch fremden Code und jede Bearbeitung ein Risiko für den Rest — in dieser Sitzung hat mir ein Skript darin schon einmal 1100 Zeilen gelöscht. Die Methoden liegen jetzt in sechzehn Dateien, die alle zur selben Klasse gehören: `SpikeWindowLayout` (Aufbau, Menüs, kompakte Seitenleiste), `SpikeWindowSession`, `SpikeWindowDownloads`, `SpikeWindowLibrary`, `SpikeWindowWorkspace`, `SpikeWindowSettings`, `SpikeWindowProfiles`, `SpikeWindowCredentials`, `SpikeWindowAssistant`, `SpikeWindowExtensions`, `SpikeWindowImport`, `SpikeWindowSync`, `SpikeWindowPasskeys`, `SpikeWindowOnboarding`, `SpikeWindowSupport` (die vormals dateilokalen Helfer) und `SpikeWindow` selbst mit den Seitenbefehlen, der Berechtigungsfläche, Darstellung, Werbeblocker, Proxy-Prüfung, Suchleiste und Schnellumschalter. Die größte Datei hat jetzt 1072 statt 6237 Zeilen. Weil alle Teile dieselbe Klasse definieren, musste kein Element öffentlich werden und keine Signatur sich ändern; `SpikeWindowInternal.hpp` trägt die gemeinsamen Includes und die Helfer-Deklarationen, und `YOBRO_SPIKE_WINDOW_SOURCES` in CMake listet die Teile einmal, damit ein neuer Teil nicht in einem der zwölf Ziele fehlt. **Es war ein reiner Umzug:** ein Zeilenvergleich vor und nach der Aufteilung zeigt null Unterschiede im Inhalt und keine verlorene Methode, und die Suite ist unverändert grün.
  **Bildschirmfreigabe funktioniert jetzt.** Die WebKit-App braucht dafür keine Zeile Code: WebKit bringt seinen eigenen Auswahldialog mit. Qt bringt keinen — es liefert über `desktopMediaRequested` zwei Modelllisten (Bildschirme und Fenster) und erwartet **genau eine** Antwort, sonst wartet die Seite endlos auf ein Versprechen, das nie eingelöst wird. Deshalb ist der Dialog hier selbst gebaut: `DesktopVideoCapture` und `DesktopAudioVideoCapture` sind jetzt auf die neuen `WebPermission::screenShare` und `screenShareWithAudio` abgebildet (vorher wurden sie still abgelehnt), die Engine-Schnittstelle hat `DesktopMediaRequest`, `DesktopMediaControls` und `setDesktopMediaHandler`, und ohne Handler wird jede Anfrage abgebrochen. `BrowserSession::onDesktopMediaRequest` hat dieselbe Torprüfung wie der Zertifikats- und der Passkey-Dialog und zusätzlich **keine privaten Tabs**: nur der aktive, sichtbare Nutzertab eines aktiven Profils darf fragen, ein Agententab niemals. Der Dialog `desktopMediaDialog` listet Bildschirme und Fenster mit Namen, „Nicht teilen“ ist die Standardschaltfläche und Escape zählt als Ablehnung, der Hinweis sagt, dass auch später davor gelegte Fenster mitgehen, und eine inzwischen verschwundene Fläche führt zum Abbruch statt zu einer falschen Freigabe.
  `chromium-screen-share` prüft alle vier Quellen im Dialog samt Beschriftung, dass Ablehnen die Anfrage beendet, dass Escape ablehnt, dass die gewählte Zeile als richtige Art und richtiger Index durchgereicht wird, dass eine verschwundene Fläche abbricht, dass eine leere Liste gar keinen Dialog öffnet — und dann **an einer echten Seite mit echtem Klick**, dass Qt 6.11.2 tatsächlich nach einem Dialog fragt (hier mit sechs Quellen, auch unsichtbar) und dass eine Ablehnung das Versprechen der Seite mit `NotAllowedError` beendet. Eine Mutationsprobe (Index fest auf 0) lässt den Test fehlschlagen.

  **Eine bewusste Abweichung beim Assistenten.** Notizen, die der Assistent schreibt, werden als reiner Text eingesetzt, nicht als HTML: Modellausgabe ist Daten, kein Markup. Die Werkzeugliste ist ansonsten dieselbe wie in der WebKit-App, inklusive `read_mail` und `read_mail_message`.

  **Drei bewusste Abweichungen bei Mail.** IMAP und SMTP laufen über dasselbe `MailWorker.py`, das die WebKit-App benutzt; `chromium-mail` prüft die Übereinstimmung per SHA-256, damit nicht zwei unterschiedlich zickige Mail-Clients entstehen. Das Postfach ist ein eigenes Fenster statt einer Überlagerung, weil jede andere Fläche in dieser Anwendung so funktioniert und der Browser daneben benutzbar bleibt. Und der Nachrichtenanzeiger ist ein `QTextBrowser` ohne jeden Netz- und Dateizugriff: entfernte Bilder, Zählpixel und entfernte Schriften laden nie. Das ist strenger als der Schalter der WebKit-App, deshalb gibt es hier keinen Schalter; die Einstellung wird trotzdem gespeichert, damit die Einstellungsdateien beider Anwendungen zusammenpassen. Mailpasswörter liegen im Schlüsselbund unter einem eigenen Dienstnamen pro Profil, nicht unter dem `local.yobro.mail` der WebKit-App, damit keine Anwendung die Postfächer der anderen lesen kann.

  Für den Import wird Python 3 gebraucht (`/usr/bin/python3` genügt, wie es macOS mit den Xcode-Befehlszeilenwerkzeugen mitbringt). Das Hilfsskript ist dasselbe wie in der WebKit-App; `chromium-password-import` prüft die Übereinstimmung per SHA-256.

  **Erweiterungen lassen sich mit Qt 6.11.2 nicht aktivieren.** `QWebEngineExtensionManager::setExtensionEnabled` stürzt bei **jedem** Aufruf ab, unabhängig vom Zeitpunkt; mit einem eigenständigen Probeprogramm bei 0, 200, 600, 1200 und 2500 ms Verzögerung gemessen, die Funktion kehrt nie zurück. Ohne Aktivierung laufen die Skripte einer Erweiterung nicht. Der Aufruf ist deshalb über `QtBrowserProfile::extensionSwitchingWorks()` stillgelegt, damit er den Browser nicht mitnimmt: Erweiterungen lassen sich installieren, werden geladen und ihr gewünschter Zustand wird gespeichert, wirken aber erst, wenn Qt das behebt. Der Erweiterungsdialog sagt das, der Ein-/Aus-Knopf ist deaktiviert, und `chromium-extension-mv3` prüft jetzt genau diesen Zustand — eine Qt-Korrektur fällt dort als Testfehler auf. Frühere Aussagen in dieser Datei, der Absturz sei umgangen, waren falsch: die Umgehung hing nur davon ab, ob der Aufruf zufällig ausblieb.

  Engine-Grenzen beim Proxy, alle drei durch `chromium-space-proxy-network` belegt: Qt WebEngine beachtet nur `QNetworkProxy::setApplicationProxy`, liest den Wert einmalig beim Aufbau des Netzwerkkontexts und kennt keine Bypass-Liste. Folge: ein Proxy pro Prozess, ein Wechsel des Space braucht einen Neustart (die Einstellungen bieten ihn an, bis dahin wird nichts geladen), und eigene Domainlisten wirken nicht — Chromium nimmt Loopback selbst aus, alles andere geht über den Proxy. Das ist mehr Verkehr über den Proxy als gefordert, nie weniger.

- [ ] **6. Plattformen und Transport**  
  macOS x86_64, Windows x64 und Linux x64 müssen gebaut und auf ihren nativen Transportwegen getestet werden.

- [ ] **7. Extensions sowie Auth/2FA**  
  Kritische Extensions und ausgewählte authentifizierte Websites müssen mit sicheren Testkonten nachgewiesen werden.

- [ ] **8. Performance und Stabilität**  
  Startup, Seitenladen, 1/10/30/100 Tabs, RAM/CPU/Energie, Freeze/Discard und Langzeitlauf müssen gemessen werden.

- [ ] **9. Release und Distribution**  
  Developer-ID-Signatur, Hardened Runtime, Notarisierung, Gatekeeper, Legal, Updater und Distributionsartefakt stehen noch aus.

- [ ] **10. Finale Gesamtregression**  
  Alle Engine-, Paket-, Paritäts-, Plattform- und Release-Gates werden frisch ausgeführt; erst danach wird der Gesamtstatus auf `PASS` gesetzt.

## Was du jetzt schon ausprobieren kannst

Den aktuellen Entwicklungsstand startest du direkt aus dem Buildverzeichnis:

```bash
cmake --build chromium/build --target yobro-chromium-spike --parallel 8
open -n "/Users/laura/Documents/APPS/Orbit/chromium/build/YOBRO Chromium Feasibility.app"
```

Die neue Qt-Quick-Shell (bewusst kleiner Funktionsumfang: Startseite, Sidebar, ein Tab mit echter Adressnavigation) liegt daneben:

```bash
cmake --build chromium/build --target yobro-chromium-qml --parallel 8
open -n "/Users/laura/Documents/APPS/Orbit/chromium/build/YOBRO Chromium QML.app"
```

Das gepackte Bundle unter `dist/` ist älter und enthält die neuesten Oberflächenänderungen noch nicht. Es wird erst nach einem vollständigen `./scripts/chromium-build.sh`-Lauf wieder aktuell.

Bereits nutzbar sind unter anderem Navigation, Tabs, Tastenkürzel, Menüleiste, Startseite, Favicons, In-Page-Suche, Quick Switcher, Split View, Tab-Umordnung per Drag-and-Drop, Spaces und Ordner, Private-Modus, Seitenzoom, Drucken, kompakte Sidebar, Website-Darkmode, Deutsch und Englisch, Einstellungen mit Profilverwaltung samt Name und Symbol, gespeicherte Logins mit AutoFill, Bibliothek mit Verlauf und Lesezeichenverwaltung, Browserdaten löschen, Downloads, MV3-Erweiterungen, Berechtigungen für Medien, Standort und Mitteilungen, HTTP-Login, Renderer-Recovery, WebKit-Import und der Agent-Bereich.

Der Funktionsumfang entspricht noch nicht der WebKit-App: Mail, der integrierte Assistent, Notizen, Synchronisierung, VPN, Adblocker und Onboarding fehlen weiterhin.

## Externe Voraussetzungen für den vollständigen Release

- macOS-x86_64-, Windows- und Linux-Runner
- freigegebene Auth-/2FA-Testkonten
- die festgelegten Extension-Artefakte
- Apple Developer-ID- und Notarisierungszugang

Diese Datei wird nach jedem abgeschlossenen Meilenstein aktualisiert.
