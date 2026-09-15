# Chromium scope decisions (v1 — user-approved)

## 1. Must-support extensions (10)

User-fixed: **Phantom (Wallet)**, **NordVPN (VPN/Proxy)**.
Rest proposed to cover every category from the migration plan; swap any of them.

| # | Extension | Kategorie | Warum Härtetest |
|---|---|---|---|
| 1 | Phantom | Wallet | Content Scripts, Toolbar-Popup, Background-Worker, Transaktions-Approval-UI |
| 2 | NordVPN 6.x | VPN/Proxy | `proxy` (pac_script), `privacy`, `webRequestAuthProvider`, `offscreen` — bekannter WebKit-Reject (s. `docs/PASSKEYS-NORDVPN.md`); entscheidender Qt-API-Coverage-Test |
| 3 | Bitwarden | Passwort-Manager | Options-Page, Autofill-Content-Scripts, Hintergrund-Sync |
| 4 | uBlock Origin | Ad-Blocker | Viele Content-Script-Regeln, WebRequest-Blocking, Performance unter Last |
| 5 | Todoist | Produktivität | Toolbar-Popup, Content-Scripts, Background-Sync |
| 6 | React Developer Tools | Entwickler-Tool | DevTools-Integration, Content-Scripts auf allen Frames |
| 7 | Keepa | Shopping | Preis-Overlays per Content-Script, Background-Fetch |
| 8 | SponsorBlock | Media | Media-State-Erkennung, Skip-Overlays |
| 9 | Dark Reader | Darstellung | CSS-Injection großflächig, Performance; bereits als `DarkReader.js`-Ressource im Repo |
| 10 | Grammarly | Schreiben | Editor-Overlays, Textfeld-Hooks, Datenschutz-sensibel |

Integrationstest für NordVPN braucht ein nutzerbedientes NordVPN-Konto
(Auth, Routing, Ausnahmen, Verbindungsabbruch) — vgl. `docs/PASSKEYS-NORDVPN.md`.

## 2. Must-support authenticated sites (5)

User: **App Store Connect** + generell alle Websites. Daraus 5 harte Auth-Fälle:

| # | Site | Auth-Muster |
|---|---|---|
| 1 | App Store Connect | Apple ID + 2FA, Session-Langlebigkeit, user/agent-Pane-Cookie-Teilung |
| 2 | Gmail | OAuth + App-Passwörter (Mail-Relevanz), Multi-Login |
| 3 | GitHub | OAuth/Device-Flow, WebAuthn/Passkey-Gegenprobe |
| 4 | iCloud Web | Apple ID + 2FA, strenge Cookie-/Storage-Anforderungen |
| 5 | Online-Banking mit 2FA (TOTP) | Strikte Session-/Frame-Policies, keine stillen Cookie-Kopien |

Prinzip aus dem Plan: explizite, previewed Cookie-/Session-Migration — niemals still kopieren.

## 3. Supported OS / CPU (v1)

| OS | Versionen | Architekturen |
|---|---|---|
| macOS | 14+ (WebKit-Referenzbaseline) | Apple Silicon (arm64) + Intel (x86_64) |
| Windows | 10 22H2+ / 11 | x64 (ARM64 später) |
| Linux | Ubuntu 22.04 LTS+, Fedora 39+ | x64 (ARM64 best-effort) |

Chromium-Preview-Datenverzeichnisse müssen pro Betriebssystem getrennt von der
WebKit-App definiert und vor Beginn eines neuen Spikes dokumentiert werden.
Perf-Baseline (Phase 0) auf Intel- + Silicon-Macs mit identischer Hardware/Seiten;
Windows-/Linux-Baselines folgen in einem neuen Spike.
