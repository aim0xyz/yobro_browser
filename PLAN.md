# Chromium UI Migration Plan

## Ziel

Die Chromium-Version soll dieselbe visuelle Sprache und dieselben zentralen
Interaktionen wie die SwiftUI/WebKit-Version von YoBro erhalten, ohne den
bereits aufgebauten Chromium-, Profil-, Sicherheits- und Agent-Unterbau neu zu
schreiben.

Die sichtbare Qt-Widgets-Oberfläche wird deshalb schrittweise durch eine
**Qt-Quick-/QML-Shell** ersetzt. Qt WebEngine bleibt die Browser-Engine; der
bestehende C++-Kern bleibt die Quelle für Profile, Sitzungen, Downloads,
History, Lesezeichen, Spaces, Sicherheit und Agent-Zugriff.

Erfolg bedeutet nicht nur, dass ein Feature vorhanden ist. Die wichtigsten
Ansichten müssen in Hell- und Dunkelmodus bei festem Fensterformat sichtbar mit
der SwiftUI-Referenz übereinstimmen.

## Architekturentscheidung

### Beibehalten

- Chromium über Qt WebEngine
- C++-Engine, BrowserSession und das Agent-Protokoll
- Profile, Speicherorte, Cookies, Downloads, Berechtigungen und Sicherheitslogik
- Datenformate und vorhandene Integrationstests

### Ersetzen

- `SpikeWindow` als sichtbare, QWidget-basierte Produkt-Shell
- sichtbare Nutzung von `QMainWindow`, `QTreeWidget`, `QTabWidget` und
  produktiven `QDialog`-Oberflächen
- große QSS-Stylesheets als primäres Design-System

### Neue Aufteilung

```text
C++ core
  BrowserSession / Profile / Downloads / Agent / Sicherheit
                 │
                 │ QObject-Models, klar begrenzte Invokables und Signale
                 ▼
Qt Quick / QML shell
  Sidebar / Tabs / Navigation / Sheets / Animationen / Design
                 │
                 ▼
Qt WebEngine `WebEngineView`
  Chromium-Seiteninhalt
```

QML darf keine Sicherheits- oder Persistenzentscheidungen besitzen. Es ist die
Darstellungsschicht; der C++-Kern validiert jede zustandsändernde Aktion.

## Arbeitsregeln

1. Keine neuen Produktfeatures im QWidget-UI. Kritische Engine- oder
   Sicherheitskorrekturen bleiben erlaubt.
2. Keine Ansicht gilt als fertig, nur weil ein Unit- oder Integrationstest grün
   ist. Sie benötigt eine visuelle Abnahme.
3. Bestehende Widgets werden nicht weiter pixelweise repariert. Neue UI-Arbeit
   findet in QML statt.
4. Immer nur eine Nutzeransicht vollständig umsetzen und abnehmen, dann die
   nächste beginnen.
5. Der aktuelle QWidget-Prototyp bleibt zunächst lauffähig als funktionale
   Referenz, bis sein QML-Ersatz die jeweilige Funktion übernommen hat.

## Phase 0 — Bestand sichern und Scope einfrieren

**Ergebnis:** Ein stabiler Ausgangspunkt ohne weitere Vergrößerung der alten UI.

- Bestehende Chromium-Tests ausführen und ihren aktuellen Stand dokumentieren.
- Funktionsoberflächen in zwei Gruppen markieren:
  - **Core:** muss erhalten bleiben (Tabs, Profile, Navigation, Downloads,
    Berechtigungen, Agent).
  - **UI:** wird ersetzt (Sidebar, Tab-Darstellung, Dialoge, Settings-Sheets).
- Alle noch offenen Feature-Aufgaben pausieren, bis die neue Shell die drei
  Kernansichten erreicht hat.
- Die Statusdokumentation eindeutig halten: Chromium-Core fortgeschritten;
  Produktoberfläche in Migration, nicht feature-par.

**Abnahme:** Der alte Build und die bestehende Testsuite bleiben reproduzierbar.

## Phase 1 — Visuelle Referenzen und Tests definieren

**Ergebnis:** Sichtbare Qualität wird messbar und nicht Geschmackssache.

Für die SwiftUI-App werden verbindliche Referenzaufnahmen erstellt, jeweils in
Hell und Dunkel und bei exakt derselben Fenstergröße und Display-Skalierung:

1. Startseite mit ausgeklappter Sidebar
2. Normale Webseite mit mehreren Tabs, aktivem Tab und Ordner
3. Eingeklappte bzw. kompakte Sidebar
4. Agent-Split-Ansicht
5. Library-Sheet
6. Settings-Sheet

Für jede Referenz wird ein deterministischer Ausgangszustand festgelegt:

- Profil und Sprache
- aktive Tabs und Tabtitel
- Sidebar- und Sheet-Zustand
- Fenstergröße
- Hell-/Dunkelmodus
- keine dynamischen Favicons, Uhrzeiten oder Netzwerkdaten

Danach entsteht ein Capture-Befehl für die QML-Version und ein Bildvergleich,
der ein Differenzbild sowie einen numerischen Wert ausgibt. Der Vergleich dient
als Warnsystem, nicht als blindes Pixelziel: Unterschiede durch Webinhalt,
Anti-Aliasing oder native Traffic Lights werden maskiert oder toleriert.

**Abnahme:** Für mindestens Startseite und normale Browseransicht liegen
SwiftUI-Referenz, Chromium-Capture und Differenzbild automatisiert vor.

## Phase 2 — QML-Fundament bauen

**Ergebnis:** Eine neue, leere QML-Anwendung kann denselben C++-Core verwenden.

- Qt-WebEngine-Integration von Widgets auf `QtWebEngine` / `WebEngineView`
  vorbereiten.
- Eine `QQmlApplicationEngine`-basierte Startstrecke ergänzen, zunächst hinter
  einem klaren Entwicklungs-Schalter oder separaten App-Target.
- Eine schmale C++-zu-QML-Brücke definieren:
  - `BrowserUiModel`: aktive Seite, URL, Titel, Ladezustand, Zurück/Vor,
    Zoom, Navigation
  - `WorkspaceModel`: Spaces, Ordner, Tabs, Reihenfolge, Pinning
  - `ProfileUiModel`: aktives Profil und Wechsel
  - `LibraryUiModel`: Verlauf, Lesezeichen, Downloads
  - `AppearanceModel`: Hell/Dunkel und Website-Darstellung
- QML-Modelle lesen überwiegend aus dem C++-Core; Aktionen gehen über wenige,
  prüfbare Methoden zurück. Keine Logik duplizieren.
- Ein gemeinsames QML-Design-System anlegen:
  - Farben und Transparenzen aus `Sources/YOBRO/Theme.swift`
  - Typografie, Spacing, Ecken, Schatten und Animation-Dauern
  - Icon-Komponente auf Basis der bereits vorhandenen Lucide-SVGs

**Abnahme:** Ein QML-Fenster zeigt ein `WebEngineView` mit realem Profil;
Navigation, Seitentitel und Ladezustand fließen korrekt durch das Modell.

## Phase 3 — Kern-Shell pixelnah umsetzen

**Ergebnis:** Die Chromium-App ist auf den wichtigsten Screenshots als YoBro
erkennbar.

Reihenfolge:

1. Fensterhintergrund, Content-Karte und macOS-Fensterchrome
2. Sidebar: Marke, Navigation, Suchfeld, Spaces, Tabliste, Aktionen,
   Profil-Footer
3. Browserfläche: Kartenrahmen, aktive Seite, Find-Bar, transienter Status
4. Sidebar-Zustände: normal, kompakt, automatisch verborgen
5. Tabs, Ordner, Pinning und Drag & Drop
6. Startseite einschließlich Suche, Karten und privatem Modus
7. Agent-Split und Rail

Für Drag & Drop, Auswahl und Tabreihenfolge wird kein verstecktes Qt-Widget als
sichtbare Liste benutzt. QML-Delegates rendern die Liste; C++ verwaltet die
echte Reihenfolge und validiert Verschiebungen.

**Abnahme:** Die sechs Referenzsituationen aus Phase 1 sind visuell geprüft;
Startseite, Standard-Shell und Sidebar-Interaktion erhalten explizite Freigabe.

## Phase 4 — Sheets und sekundäre Ansichten migrieren

**Ergebnis:** Die restliche sichtbare Oberfläche folgt einem einheitlichen
System statt vielen individuell gestylten Dialogen.

Migration in dieser Reihenfolge:

1. Library und Downloads
2. Einstellungen und Profile
3. Onboarding
4. Login-AutoFill und Passwortverwaltung
5. Mail
6. Assistent und Agent-Aktivität
7. Import, Erweiterungen und Passkeys

Alle davon verwenden dieselbe QML-Sheet-Komponente für Hintergrund,
Material/Overlay, Titel, Fußzeile, Fokus, Escape und Animation. Plattformnahe
Systemdialoge bleiben dort nativ, wo das bewusst erwünscht ist, etwa beim
Dateiauswahldialog oder Drucken.

**Abnahme:** Keine produktiv sichtbare Oberfläche hängt mehr an einem
`QDialog`- oder `QTabWidget`-Layout, sofern sie nicht absichtlich eine native
Systemoberfläche ist.

## Phase 5 — Verhalten, Zugänglichkeit und Plattformprüfung

**Ergebnis:** Schöne Screenshots bleiben eine stabile, bedienbare App.

- Tastaturkürzel, Fokusreihenfolge und Screenreader-Namen prüfen.
- High-DPI, kleine und große Fenster sowie macOS- und Windows-Skalierung
  prüfen.
- Kontrast, Hover-, Pressed-, Disabled- und Auswahlzustände in Hell und Dunkel
  prüfen.
- QML- und WebEngine-Speicher/Lebensdauer testen: Tab schließen, Profilwechsel,
  private Tabs, Renderer-Crash und App-Beenden.
- Windows-spezifische Abstände und Systemschrift bewusst testen, ohne das
  macOS-Design durch Windows-Standardwidgets zu ersetzen.

**Abnahme:** Kern-Integrationstests und die visuellen Referenztests laufen auf
macOS; ein Windows-Build zeigt Shell und Browserinhalt korrekt bei High-DPI.

## Phase 6 — Umschalten und aufräumen

**Ergebnis:** QML ist die Produktoberfläche, ohne tote Parallelimplementierung.

- QML-Shell zum Standard-Startpfad machen.
- Erst nach abgeschlossener Funktions- und Screenshot-Abnahme die ersetzten
  QWidget-Oberflächen entfernen.
- Core-Code, der nur wegen des alten Widget-Lebenszyklus existierte, gezielt
  vereinfachen.
- Packaging für QML-Imports, Qt-WebEngine-Prozess, Ressourcen, Icons und
  Übersetzungen auf macOS und Windows prüfen.
- `chromium/STATUS.md` auf realen Produktstand und bekannte Engine-Grenzen
  aktualisieren.

**Abnahme:** Die App wird aus dem QML-Startpfad paketiert, startet ohne
Entwicklungsdateien und besteht Core-, Interaktions- und visuelle Tests.

## Nichtziele

- Kein Neuaufbau von Chromium mit CEF oder Electron während dieser Migration.
- Kein 1:1-Kopieren jedes Plattformdetails, wenn es Zugänglichkeit oder
  Bedienbarkeit verschlechtert.
- Keine neue Funktionsparität als Ersatz für visuelle Qualität.
- Keine Entfernung des bewährten C++-Cores ohne einen konkreten technischen
  Grund.

## Erste umsetzbare Lieferung

Die erste Lieferung soll bewusst klein sein:

1. QML-App-Target mit realem `WebEngineView`
2. Token-Datei und Icon-Komponente
3. Statische Sidebar und Browser-Content-Karte
4. reale Adressnavigation sowie ein Tab
5. Capture für die Startseite in Hell und Dunkel

**Stand, 16. September 2026:** Diese fünf Punkte sind umgesetzt und geprüft
(`yobro-chromium-qml`, Tests `chromium-qml-shell-capture` und
`chromium-qml-shell-navigation`, Details in `chromium/STATUS.md`). Offen aus
Phase 1 bleiben die verbindlichen neuen SwiftUI-Referenzaufnahmen im heutigen
App-Stand; die vorhandenen Aufnahmen zeigen noch das ältere Layout.

Danach wird anhand der Referenzbilder entschieden, ob Layout, Typografie und
Rendering den gewünschten Qualitätsgrad erreichen, bevor weitere Features
migriert werden.
