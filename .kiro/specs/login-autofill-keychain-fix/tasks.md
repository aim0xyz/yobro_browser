# Implementierungsplan

- [ ] 1. Bug-Condition-Explorationstest schreiben
  - **Property 1: Bug Condition** – Password AutoFill wird nicht angeboten
  - **WICHTIG**: Diesen Test VOR der Umsetzung des Fixes schreiben.
  - **KRITISCH**: Dieser Test MUSS auf dem ungefixten Code FEHLSCHLAGEN – der Fehlschlag bestätigt, dass der Bug existiert.
  - **NICHT** versuchen, den Test oder den Code zu reparieren, wenn er fehlschlägt.
  - **HINWEIS**: Dieser Test kodiert das erwartete Verhalten – er validiert später den Fix, wenn er nach der Umsetzung PASST.
  - **ZIEL**: Gegenbeispiele erzeugen, die den Bug sichtbar machen (aus "Bug Condition" / "Exploratory Bug Condition Checking" im Design).
  - **Scoped-PBT-Ansatz**: Die Property auf die konkreten deterministischen Fälle beschränken – die `textContentType`-Zuordnung der Login-Felder in `SyncSettings` (`Sources/YOBRO/BrowserSettings.swift`, `else`-Zweig / nicht angemeldet).
  - In `Tests/Swift` einen Test hinzufügen, der prüft: für das Login-Formular gilt E-Mail-Feld == `.username` UND Passwort-`SecureField` == `.password` (entspricht `isBugCondition(input) = focus in {emailField, passwordField} AND keychainSuggestionsOffered = false`).
  - Test auf UNGEFIXTEM Code ausführen: E-Mail-Feld ist `.emailAddress` (nicht `.username`), Passwortfeld hat KEINEN `.textContentType(.password)`.
  - **ERWARTETES ERGEBNIS**: Test SCHLÄGT FEHL (korrekt – beweist, dass der Bug existiert).
  - Gefundene Gegenbeispiele dokumentieren (z. B. "Login-`SecureField` ohne `.textContentType(.password)`", "E-Mail-Feld `.emailAddress` statt `.username` → kein erkanntes Login-Paar").
  - Aufgabe abschließen, wenn der Test geschrieben, ausgeführt und der Fehlschlag dokumentiert ist.
  - _Requirements: 1.1, 1.2, 1.3, 1.4_

- [ ] 2. Preservation-Property-Tests schreiben (VOR der Umsetzung des Fixes)
  - **Property 2: Preservation** – Unverändertes Verhalten außerhalb der Bug-Bedingung
  - **WICHTIG**: Observation-First-Methodik anwenden (Verhalten zuerst auf UNGEFIXTEM Code beobachten und festhalten).
  - Beobachten: "Anmelden" ruft `sync.signIn(email:password:recoveryCode:model:)` mit den manuell eingegebenen Werten auf und setzt anschließend `password = ""; recovery = ""`.
  - Beobachten: "Konto erstellen" ruft `sync.signUp(email:password:model:)` auf und setzt `password = ""`.
  - Beobachten: Passwortfeld bleibt ein `SecureField` (maskierte Eingabe); Recovery-Feld ist ein `SecureField` OHNE Passwort-Content-Type.
  - Beobachten: "Bestätigung erneut senden" und "Passwort vergessen?" funktionieren wie bisher.
  - Property-basierte Tests schreiben (aus "Preservation Checking" / "Property-Based Tests" im Design): für zufällige `(email, password)`-Paare bleibt der `sync.signIn`/`sync.signUp`-Aufruf mit denselben Werten unverändert; das Recovery-Feld erhält KEINEN Passwort-Content-Type.
  - Tests auf UNGEFIXTEM Code ausführen.
  - **ERWARTETES ERGEBNIS**: Tests PASSEN (bestätigt das zu erhaltende Basisverhalten).
  - Aufgabe abschließen, wenn die Tests geschrieben, ausgeführt und auf ungefixtem Code grün sind.
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

- [ ] 3. Fix für fehlende Password-AutoFill-/Schlüsselbund-Vorschläge im Login-Formular

  - [ ] 3.1 Content-Type-Zuordnung der Login-Felder korrigieren
    - In `Sources/YOBRO/BrowserSettings.swift`, `SyncSettings`, Login-Zweig (`else`-Block, nicht angemeldet):
    - Dem Login-`SecureField` (`text: $password`) den Modifier `.textContentType(.password)` hinzufügen.
    - Das E-Mail-`TextField` von `.textContentType(.emailAddress)` auf `.textContentType(.username)` umstellen, damit E-Mail + Passwort als Login-Paar (Username + Password) erkannt werden.
    - Sicherstellen, dass das `.username`-Feld und das `.password`-Feld benachbart im selben `VStack` liegen; bestehende Reihenfolge (E-Mail, Passwort, Recovery) beibehalten.
    - Das Recovery-`SecureField` (`text: $recovery`) ausdrücklich NICHT mit `.textContentType(.password)` markieren – bleibt unverändert.
    - _Bug_Condition: isBugCondition(input) = input.field in {emailField, passwordField} AND screenState = signedOut AND keychainSuggestionsOffered = false_
    - _Expected_Behavior: expectedBehavior(result) = keychainSuggestionsOffered(result) = true (Property 1 im Design)_
    - _Preservation: Preservation Requirements aus dem Design (manuelle Anmeldung, Kontoerstellung, maskiertes Passwort, Recovery-Feld, Hilfsaktionen)_
    - _Requirements: 2.1, 2.2, 2.3_

  - [ ] 3.2 Zugangsdaten nach erfolgreicher Anmeldung im Schlüsselbund speichern/aktualisieren (optional)
    - Nach erfolgreichem `sync.signIn` die eingegebenen Zugangsdaten idempotent als Generic-Password-Eintrag (Keychain Services) für den App-Service speichern/aktualisieren.
    - Speichern nur auslösen, wenn die Anmeldung erfolgreich war (z. B. `sync.signedIn == true` bzw. kein Fehlerstatus); bei fehlgeschlagener Anmeldung nichts speichern.
    - Bestehenden UI-Reset (`password = ""; recovery = ""`) unverändert beibehalten.
    - _Bug_Condition: isBugCondition beim erfolgreichen Login (keychainSuggestionsOffered = false, kein Speicherangebot)_
    - _Expected_Behavior: Nach erfolgreicher Anmeldung wird das Speichern/Aktualisieren der Zugangsdaten angeboten/durchgeführt (Property 1 im Design)_
    - _Preservation: UI-Reset und `sync.signIn`-Aufruf bleiben unverändert_
    - _Requirements: 2.4_

  - [ ] 3.3 Verifizieren, dass der Bug-Condition-Explorationstest jetzt passt
    - **Property 1: Expected Behavior** – Password AutoFill wird angeboten
    - **WICHTIG**: Denselben Test aus Aufgabe 1 erneut ausführen – KEINEN neuen Test schreiben.
    - Der Test aus Aufgabe 1 kodiert das erwartete Verhalten; besteht er, ist das erwartete Verhalten erfüllt.
    - Bug-Condition-Explorationstest aus Schritt 1 ausführen: E-Mail-Feld == `.username`, Passwortfeld == `.password`.
    - **ERWARTETES ERGEBNIS**: Test PASST (bestätigt, dass der Bug behoben ist).
    - _Requirements: 2.1, 2.2, 2.3, 2.4_

  - [ ] 3.4 Verifizieren, dass die Preservation-Tests weiterhin passen
    - **Property 2: Preservation** – Unverändertes Verhalten außerhalb der Bug-Bedingung
    - **WICHTIG**: Dieselben Tests aus Aufgabe 2 erneut ausführen – KEINE neuen Tests schreiben.
    - Preservation-Property-Tests aus Schritt 2 ausführen.
    - **ERWARTETES ERGEBNIS**: Tests PASSEN (keine Regressionen): manuelle Anmeldung, Kontoerstellung, maskiertes `SecureField`, Recovery-Feld ohne Passwort-Content-Type, "Bestätigung erneut senden" und "Passwort vergessen?" unverändert.
    - Bestätigen, dass alle Tests nach dem Fix weiterhin grün sind.
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5_

- [ ] 4. Checkpoint – Sicherstellen, dass alle Tests bestehen
  - Vollständige Testsuite (`swift test` bzw. das relevante Swift-Testtarget in `Tests/Swift`) ausführen und sicherstellen, dass alle Tests bestehen.
  - Manuell/integrativ auf dem Gerät prüfen: Beim Fokussieren von E-Mail-/Passwortfeld werden Schlüsselbund-Vorschläge angeboten; nach erfolgreicher Anmeldung wird Speichern/Aktualisieren angeboten (Property 1).
  - Bei Unklarheiten oder fehlschlagenden Tests den Nutzer fragen.
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 3.1, 3.2, 3.3, 3.4, 3.5_
