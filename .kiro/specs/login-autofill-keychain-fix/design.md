# Login AutoFill / Schlüsselbund-Fix – Bugfix Design

## Overview

Auf dem Login-Bildschirm der Konto-Synchronisierung (`SyncSettings` in `Sources/YOBRO/BrowserSettings.swift`) bietet das System keine Password-AutoFill-/Schlüsselbund-Vorschläge an, weil das Passwortfeld keinen `.textContentType(.password)` besitzt und E-Mail- und Passwortfeld vom System nicht zuverlässig als zusammengehöriges Login-Formular erkannt werden. Zusätzlich werden nach einer erfolgreichen Anmeldung keine Zugangsdaten im Schlüsselbund gespeichert oder aktualisiert.

Der Fix ist bewusst minimal und zielgerichtet: Er korrigiert ausschließlich die Content-Type-Zuordnung der beiden Login-Felder, sorgt dafür, dass die Felder als Login-Formular (Username + Password) erkennbar werden, und ergänzt optional das Speichern/Aktualisieren der Zugangsdaten im Schlüsselbund nach erfolgreicher Anmeldung. Alle übrigen Verhaltensweisen (manuelle Anmeldung, Kontoerstellung, maskiertes Passwort, Wiederherstellungscode, "Bestätigung erneut senden", "Passwort vergessen?") bleiben unverändert.

## Glossary

- **Bug_Condition (C)**: Der Nutzer fokussiert ein Login-Feld (E-Mail oder Passwort) auf dem Anmeldebildschirm, und das System bietet dabei keine Schlüsselbund-Vorschläge an.
- **Property (P)**: Beim Fokussieren eines Login-Feldes bietet das System Password-AutoFill-/Schlüsselbund-Vorschläge an; nach erfolgreicher Anmeldung wird das Speichern/Aktualisieren der Zugangsdaten angeboten.
- **Preservation**: Bestehendes Verhalten, das durch den Fix unverändert bleiben muss – manuelle Anmeldung, Kontoerstellung, maskierte Passwortdarstellung, Wiederherstellungscode-Feld sowie die Aktionen "Bestätigung erneut senden" und "Passwort vergessen?".
- **`SyncSettings`**: Die SwiftUI-View in `Sources/YOBRO/BrowserSettings.swift`, die den Login-/Sync-Bildschirm rendert. Der Login-Zweig liegt im `else`-Block (Nutzer nicht angemeldet).
- **`BrowserSyncStore` (`sync`)**: Das ObservableObject, das `signIn(email:password:recoveryCode:model:)`, `signUp(...)`, `signedIn`, `email` usw. bereitstellt.
- **emailField**: Das `TextField(L("E-Mail", "Email"), text: $email)` im Login-Zweig.
- **passwordField**: Das `SecureField(L("Passwort ...", ...), text: $password)` im Login-Zweig.
- **recoveryField**: Das `SecureField(L("Wiederherstellungscode ...", ...), text: $recovery)` – KEIN Login-Passwortfeld, muss unverändert bleiben.
- **textContentType**: SwiftUI-Modifier, der dem System die semantische Rolle eines Eingabefeldes mitteilt (`.username`, `.password`, `.emailAddress`) und damit Password AutoFill steuert.

## Bug Details

### Bug Condition

Der Bug tritt auf, wenn der Nutzer im nicht angemeldeten Zustand das E-Mail- oder das Passwortfeld des Login-Formulars fokussiert. Das Passwortfeld (`SecureField`) besitzt keinen `.textContentType(.password)`, und das E-Mail-Feld ist zwar mit `.emailAddress` markiert, wird aber ohne ein korrespondierendes Passwortfeld mit korrektem Content-Type nicht als Teil eines Login-Formulars (Username + Password) erkannt. Dadurch bietet das System keine Schlüsselbund-Vorschläge an.

**Formal Specification:**
```
FUNCTION isBugCondition(input)
  INPUT: input of type LoginFieldFocus
  OUTPUT: boolean

  RETURN input.field IN { emailField, passwordField }
         AND input.screenState = signedOut
         AND keychainSuggestionsOffered(input) = false
END FUNCTION
```

### Examples

- Nutzer öffnet den Sync-Login, tippt ins E-Mail-Feld → erwartet: gespeicherte Zugangsdaten werden als AutoFill-Vorschlag angeboten; tatsächlich: keine Vorschläge.
- Nutzer tippt ins Passwortfeld → erwartet: Passwort-Vorschlag aus dem Schlüsselbund; tatsächlich: keine Vorschläge (fehlender `.textContentType(.password)`).
- Für die App sind Zugangsdaten im Schlüsselbund gespeichert → erwartet: E-Mail und Passwort können per AutoFill ausgefüllt werden; tatsächlich: kein Ausfüllen möglich.
- Nutzer meldet sich erfolgreich an → erwartet: System bietet an, Zugangsdaten zu speichern/aktualisieren; tatsächlich: kein Angebot.
- Edge Case: Wiederherstellungscode-Feld → erwartet: wird NICHT als Passwort behandelt, keine AutoFill-Kopplung; muss unverändert bleiben.

## Expected Behavior

### Preservation Requirements

**Unchanged Behaviors:**
- Manuelle Eingabe von E-Mail und Passwort und Anmeldung über "Anmelden" funktioniert weiterhin wie bisher (`sync.signIn`).
- "Konto erstellen" funktioniert weiterhin wie bisher (`sync.signUp`).
- Das Passwortfeld stellt die Eingabe weiterhin maskiert dar (bleibt ein `SecureField`).
- Wiederherstellungscode-Feld sowie die Aktionen "Bestätigung erneut senden" und "Passwort vergessen?" funktionieren weiterhin wie bisher.
- Wenn AutoFill-Vorschläge nicht genutzt werden, bleibt die manuelle Eingabe unverändert möglich.

**Scope:**
Alle Eingaben und Interaktionen, die NICHT das Fokussieren des E-Mail- oder Passwortfeldes des Login-Formulars betreffen, bleiben durch diesen Fix vollständig unberührt. Dazu gehören:
- Das Wiederherstellungscode-Feld (`recoveryField`)
- Der Formularzustand "angemeldet" (`sync.signedIn`) und "neues Passwort festlegen" (`sync.needsNewPassword`)
- Alle Buttons und deren `disabled`-Bedingungen, Statusanzeige und Layout

**Note:** Das konkret erwartete korrekte Verhalten (Vorschläge werden angeboten) ist in der Sektion "Correctness Properties" (Property 1) definiert.

## Hypothesized Root Cause

Auf Basis der Bug-Beschreibung und des gelesenen Codes sind die wahrscheinlichsten Ursachen:

1. **Fehlender Passwort-Content-Type**: Das Login-`SecureField` besitzt keinen `.textContentType(.password)`. Ohne diese semantische Markierung erkennt das System das Feld nicht als Passwortfeld und bietet keine Schlüsselbund-Vorschläge an.

2. **Unvollständige Login-Formular-Erkennung**: Das E-Mail-Feld nutzt `.emailAddress` statt `.username`. Für Password AutoFill koppelt das System typischerweise ein `.username`-Feld mit einem direkt folgenden `.password`-Feld. Ohne dieses Paar wird das Formular nicht als Login-Formular klassifiziert.

3. **Fehlendes Speichern nach erfolgreichem Login**: Nach `sync.signIn` wird das Passwort im UI zurückgesetzt (`password = ""`), es erfolgt jedoch kein Angebot bzw. keine Übergabe der Zugangsdaten an den Schlüsselbund. Daher wird das Speichern/Aktualisieren nicht angeboten.

4. **Mögliche Störung durch das Recovery-Feld**: Das direkt nachfolgende Wiederherstellungscode-`SecureField` darf NICHT als Passwortfeld markiert werden, sonst könnte die AutoFill-Kopplung des eigentlichen Login-Paares gestört werden.

## Correctness Properties

Property 1: Bug Condition – Password AutoFill wird angeboten

_For any_ Eingabe, bei der die Bug-Bedingung gilt (isBugCondition liefert true) – also das E-Mail- oder Passwortfeld des Login-Formulars im nicht angemeldeten Zustand fokussiert wird – SOLL das gefixte Formular Password-AutoFill-/Schlüsselbund-Vorschläge anbieten, sodass gespeicherte Zugangsdaten vorgeschlagen und per AutoFill ausgefüllt werden können. Zusätzlich SOLL nach erfolgreicher Anmeldung das Speichern/Aktualisieren der eingegebenen Zugangsdaten im Schlüsselbund angeboten werden.

**Validates: Requirements 2.1, 2.2, 2.3, 2.4**

Property 2: Preservation – Unverändertes Verhalten außerhalb der Bug-Bedingung

_For any_ Eingabe, bei der die Bug-Bedingung NICHT gilt (isBugCondition liefert false) – manuelle Anmeldung, Kontoerstellung, maskierte Passwortdarstellung, Nutzung des Wiederherstellungscode-Feldes sowie "Bestätigung erneut senden" und "Passwort vergessen?" – SOLL das gefixte Formular dasselbe beobachtbare Ergebnis liefern wie das ursprüngliche Formular und alle bestehenden Funktionen unverändert erhalten.

**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5**

## Fix Implementation

### Changes Required

Unter der Annahme, dass die Root-Cause-Analyse korrekt ist:

**File**: `Sources/YOBRO/BrowserSettings.swift`

**View / Funktion**: `SyncSettings` – Login-Zweig im `else`-Block (nicht angemeldet)

**Specific Changes**:

1. **Passwortfeld als Passwort markieren**: Dem Login-`SecureField` den Modifier `.textContentType(.password)` hinzufügen.
   ```swift
   SecureField(L("Passwort (mindestens 6 Zeichen)", "Password (at least 6 characters)"), text: $password)
       .textContentType(.password)
   ```

2. **E-Mail-Feld für Username-AutoFill markieren**: Content-Type des E-Mail-Feldes auf `.username` setzen, damit E-Mail + Passwort als Login-Paar erkannt werden.
   ```swift
   TextField(L("E-Mail", "Email"), text: $email)
       .textContentType(.username)
   ```
   (Alternativ, falls E-Mail-Semantik erhalten bleiben soll und die Plattform es zulässt, kann `.username` bevorzugt werden, da es für die Login-Formular-Erkennung entscheidend ist.)

3. **Login-Felder als Gruppe erkennbar machen**: Sicherstellen, dass das `.username`-Feld und das `.password`-Feld benachbart im selben Container (`VStack`) liegen und nicht durch das Recovery-Feld getrennt werden, damit die OS-Kopplung greift. Die bestehende Reihenfolge (E-Mail, Passwort, Recovery) erfüllt dies bereits; die Reihenfolge bleibt unverändert.

4. **Recovery-Feld ausdrücklich NICHT als Passwort markieren**: Das `recoveryField` (`SecureField` für den Wiederherstellungscode) erhält KEINEN `.textContentType(.password)`, damit die Login-Kopplung nicht gestört wird. Es bleibt vollständig unverändert.

5. **Zugangsdaten nach erfolgreicher Anmeldung speichern (optional, für 2.4)**: Nach erfolgreichem `sync.signIn` die Zugangsdaten im Schlüsselbund speichern/aktualisieren. Auf macOS erfolgt dies durch Persistieren eines Generic-Password-Eintrags (Keychain Services) für den App-Service. Der Aufruf wird nur ausgelöst, wenn die Anmeldung erfolgreich war (`sync.signedIn == true` bzw. kein Fehlerstatus), und ersetzt/aktualisiert einen bestehenden Eintrag idempotent. Der bestehende UI-Reset (`password = ""; recovery = ""`) bleibt erhalten.

## Testing Strategy

### Validation Approach

Zweiphasig: Zuerst werden Gegenbeispiele erzeugt, die den Bug auf dem ungefixten Code sichtbar machen, danach wird verifiziert, dass der Fix wirkt und bestehendes Verhalten erhalten bleibt. Da AutoFill-Verhalten stark vom Betriebssystem gesteuert wird, konzentrieren sich die automatisierten Prüfungen auf verifizierbare Merkmale: die korrekten `textContentType`-Zuordnungen der Login-Felder, die unveränderte Struktur (SecureField-Maskierung, Recovery-Feld ohne Passwort-Content-Type) und die Speicher-Logik nach erfolgreicher Anmeldung. Das tatsächliche Anzeigen der Vorschläge wird ergänzend manuell/integrativ auf dem Gerät geprüft.

### Exploratory Bug Condition Checking

**Goal**: Gegenbeispiele erzeugen, die den Bug VOR dem Fix zeigen, und die Root-Cause-Analyse bestätigen oder widerlegen. Bei Widerlegung wird neu hypothesiert.

**Test Plan**: Die Login-Felder auf ihre `textContentType`-Zuordnung prüfen und manuell verifizieren, dass beim Fokussieren keine Schlüsselbund-Vorschläge erscheinen. Auf dem UNGEFIXTEN Code ausführen, um das Fehlverhalten zu beobachten.

**Test Cases**:
1. **Passwortfeld-Content-Type**: Prüfen, dass das Login-`SecureField` KEINEN `.password`-Content-Type hat (schlägt auf ungefixtem Code fehl im Sinne von "Bug reproduziert").
2. **E-Mail/Username-Kopplung**: Prüfen, dass das E-Mail-Feld nicht als `.username` markiert ist und daher kein Login-Paar bildet (Bug reproduziert).
3. **Manueller AutoFill-Test**: Auf dem Gerät das E-Mail-/Passwortfeld fokussieren und beobachten, dass keine Schlüsselbund-Vorschläge erscheinen.
4. **Speichern nach Login**: Erfolgreich anmelden und beobachten, dass kein Speichern/Aktualisieren angeboten wird (Bug reproduziert).

**Expected Counterexamples**:
- Login-`SecureField` ohne `.textContentType(.password)`.
- E-Mail-Feld mit `.emailAddress` statt `.username`, kein erkanntes Login-Paar.
- Mögliche Ursachen: fehlender Passwort-Content-Type, fehlende Username-Kopplung, kein Keychain-Speichern nach Login.

### Fix Checking

**Goal**: Verifizieren, dass für alle Eingaben, bei denen die Bug-Bedingung gilt, das gefixte Formular das erwartete Verhalten zeigt.

**Pseudocode:**
```
FOR ALL input WHERE isBugCondition(input) DO
  result := focusField_fixed(input)
  ASSERT keychainSuggestionsOffered(result) = true
END FOR
```

### Preservation Checking

**Goal**: Verifizieren, dass für alle Eingaben, bei denen die Bug-Bedingung NICHT gilt, das gefixte Formular dasselbe Ergebnis liefert wie das ursprüngliche.

**Pseudocode:**
```
FOR ALL input WHERE NOT isBugCondition(input) DO
  ASSERT focusField_original(input) = focusField_fixed(input)
END FOR
```

**Testing Approach**: Property-based Testing eignet sich für das Preservation-Checking, weil es viele Eingaben automatisch über den Eingabebereich erzeugt, Randfälle findet und starke Garantien für unverändertes Verhalten aller Nicht-Bug-Eingaben liefert.

**Test Plan**: Verhalten zuerst auf dem UNGEFIXTEN Code für manuelle Anmeldung, Kontoerstellung, Recovery-Feld und die Hilfsaktionen beobachten, dann Tests schreiben, die dieses Verhalten festhalten.

**Test Cases**:
1. **Manuelle Anmeldung**: Verifizieren, dass `sync.signIn` mit manuell eingegebenen Werten weiterhin wie bisher aufgerufen wird und dieselben Effekte hat.
2. **Kontoerstellung**: Verifizieren, dass `sync.signUp` unverändert funktioniert.
3. **Maskierung**: Verifizieren, dass das Passwortfeld ein `SecureField` bleibt (Eingabe maskiert).
4. **Recovery / Hilfsaktionen**: Verifizieren, dass Wiederherstellungscode-Feld, "Bestätigung erneut senden" und "Passwort vergessen?" unverändert funktionieren und das Recovery-Feld KEINEN Passwort-Content-Type hat.

### Unit Tests

- Verifizieren der `textContentType`-Zuordnung: E-Mail-Feld → `.username`, Passwortfeld → `.password`, Recovery-Feld → kein Passwort-Content-Type.
- Verifizieren, dass das Passwortfeld ein `SecureField` (maskiert) bleibt.
- Verifizieren der Keychain-Speicherlogik: nach erfolgreichem Login wird ein Eintrag idempotent gespeichert/aktualisiert; bei fehlgeschlagenem Login wird nichts gespeichert.

### Property-Based Tests

- Zufällige (email, password)-Paare erzeugen und verifizieren, dass die manuelle Anmeldung `sync.signIn` unverändert mit denselben Werten aufruft (Preservation).
- Zufällige Nicht-Login-Interaktionen (Recovery-Eingaben, Hilfsaktionen) erzeugen und verifizieren, dass Original- und gefixtes Verhalten übereinstimmen.
- Idempotenz der Keychain-Speicherung über viele aufeinanderfolgende Logins prüfen.

### Integration Tests

- Vollständiger Login-Flow auf dem Gerät: E-Mail-/Passwortfeld fokussieren und beobachten, dass Schlüsselbund-Vorschläge angeboten werden (Property 1).
- Erfolgreiche Anmeldung durchführen und prüfen, dass das Speichern/Aktualisieren der Zugangsdaten angeboten bzw. durchgeführt wird.
- Umschalten zwischen den Formularzuständen (nicht angemeldet → angemeldet → neues Passwort) und Prüfen, dass Layout und Verhalten unverändert bleiben.
