# Bugfix Requirements Document

## Introduction

Auf dem Login-Bildschirm (Konto-Anmeldung für die Synchronisierung, `BrowserSettings.swift`) erscheinen beim Fokussieren der E-Mail- und Passwortfelder keine AutoFill-Vorschläge aus dem Schlüsselbund (Password AutoFill). Nutzer erhalten weder vorausgefüllte noch vorgeschlagene gespeicherte Zugangsdaten und werden nach einer erfolgreichen Anmeldung auch nicht gefragt, ob die Zugangsdaten gespeichert werden sollen.

Ursache-relevanter Befund: Das E-Mail-Feld setzt zwar `.textContentType(.emailAddress)`, das Passwortfeld (`SecureField`) hat jedoch **keinen** `.textContentType(.password)` und die Felder sind keiner logischen Login-Gruppe zugeordnet. Ohne korrekte Content-Type-Zuordnung erkennt das System das Formular nicht als Login-Formular und bietet keine Schlüsselbund-Vorschläge an. Zusätzlich fehlt nach erfolgreicher Anmeldung ein Speichern der Zugangsdaten im Schlüsselbund.

Dieses Dokument beschreibt ausschließlich das beobachtbare Verhalten (Ist/Soll/Unverändert). Technische Umsetzungsdetails folgen im Design-Dokument.

## Bug Analysis

### Current Behavior (Defect)

1.1 WENN der Nutzer das E-Mail-Feld des Login-Bildschirms fokussiert, DANN zeigt das System keine gespeicherten Zugangsdaten aus dem Schlüsselbund als AutoFill-Vorschlag an
1.2 WENN der Nutzer das Passwortfeld des Login-Bildschirms fokussiert, DANN zeigt das System keine gespeicherten Zugangsdaten aus dem Schlüsselbund als AutoFill-Vorschlag an
1.3 WENN gespeicherte Zugangsdaten für die App im Schlüsselbund vorhanden sind, DANN werden E-Mail und Passwort nicht vorausgefüllt oder vorgeschlagen
1.4 WENN sich der Nutzer erfolgreich anmeldet, DANN bietet das System nicht an, die eingegebenen Zugangsdaten im Schlüsselbund zu speichern oder zu aktualisieren

### Expected Behavior (Correct)

2.1 WENN der Nutzer das E-Mail-Feld des Login-Bildschirms fokussiert, DANN SOLL das System gespeicherte Zugangsdaten aus dem Schlüsselbund als AutoFill-Vorschlag anbieten
2.2 WENN der Nutzer das Passwortfeld des Login-Bildschirms fokussiert, DANN SOLL das System gespeicherte Zugangsdaten aus dem Schlüsselbund als AutoFill-Vorschlag anbieten
2.3 WENN gespeicherte Zugangsdaten für die App im Schlüsselbund vorhanden sind, DANN SOLL das System das Ausfüllen von E-Mail und Passwort über Password AutoFill ermöglichen
2.4 WENN sich der Nutzer erfolgreich anmeldet, DANN SOLL das System anbieten, die eingegebenen Zugangsdaten im Schlüsselbund zu speichern oder zu aktualisieren

### Unchanged Behavior (Regression Prevention)

3.1 WENN der Nutzer E-Mail und Passwort manuell eingibt und auf "Anmelden" tippt, DANN SOLL das System die Anmeldung WEITERHIN wie bisher durchführen
3.2 WENN der Nutzer auf "Konto erstellen" tippt, DANN SOLL das System die Kontoerstellung WEITERHIN wie bisher durchführen
3.3 WENN der Nutzer das Passwortfeld nutzt, DANN SOLL das System die Eingabe WEITERHIN maskiert (als sicheres Feld) darstellen
3.4 WENN das E-Mail-Feld, das Wiederherstellungscode-Feld sowie die Aktionen "Bestätigung erneut senden" und "Passwort vergessen?" genutzt werden, DANN SOLLEN diese WEITERHIN wie bisher funktionieren
3.5 WENN der Nutzer AutoFill-Vorschläge nicht nutzt, DANN SOLL die manuelle Eingabe von Zugangsdaten WEITERHIN unverändert möglich sein

## Bug Condition (zur Ableitung)

```pascal
FUNCTION isBugCondition(X)
  INPUT: X of type LoginFieldFocus
  OUTPUT: boolean

  // Fehler tritt auf, wenn ein Login-Feld (E-Mail oder Passwort) fokussiert
  // wird und dabei keine Schlüsselbund-Vorschläge angeboten werden
  RETURN X.field IN {emailField, passwordField}
END FUNCTION
```

```pascal
// Property: Fix Checking – Password AutoFill wird angeboten
FOR ALL X WHERE isBugCondition(X) DO
  result ← focusField'(X)
  ASSERT keychainSuggestionsOffered(result) = true
END FOR
```

```pascal
// Property: Preservation Checking – unveränderte Pfade bleiben gleich
FOR ALL X WHERE NOT isBugCondition(X) DO
  ASSERT focusField(X) = focusField'(X)
END FOR
```
