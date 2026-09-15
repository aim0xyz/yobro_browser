# Cross-platform browser sync

YOBRO uses a backend-neutral, local-first sync design for future macOS, Windows, Linux, iOS and Android clients. iCloud-only sync is therefore unsuitable.

The versioned snapshot and service boundary are implemented in `BrowserSync.swift`. Snapshots contain tabs, spaces, folders, bookmarks and sanitized history. They explicitly omit cookies, website sessions, passwords, mail accounts, downloads, agent-control endpoints and favicon bytes. Payloads are encrypted client-side with ChaCha20-Poly1305 before a backend sees them. A revision check rejects concurrent overwrites instead of silently losing data.

`SupabaseSyncService.swift` is the first adapter and `supabase/migrations/20260907000000_browser_sync.sql` defines the RLS-protected database contract. The Free project `YOBRO` is provisioned in Frankfurt under the separate YOBRO organization. RLS and four owner-only policies are active; authenticated clients can call the optimistic-revision RPC while anonymous clients cannot.

The settings app now provides email/password authentication, refresh-token persistence and per-profile encryption keys in the macOS Keychain. A URL-safe recovery code moves the encryption key to another device without giving Supabase plaintext access. Local changes are uploaded after a short debounce and remote state is pulled at launch or on demand. Snapshot timestamps provide last-write-wins behavior when devices diverge; server revisions prevent a stale request from silently overwriting a newer request.

Cookies and website sessions, passwords, mail accounts, downloads, extensions, proxies and favicon bytes remain device-local. The first end-to-end user run still requires creating or signing into a YOBRO account in Settings > Sync and safely recording the recovery code. Future iOS and Windows clients can reuse the REST contract and recovery code without depending on iCloud.

## Auth links and email delivery

The hosted Auth configuration uses `yobro://auth/callback` as its Site URL and allows both `yobro://auth/callback` and `yobro://auth/reset-password`. The macOS bundle registers the `yobro` scheme. Confirmation and recovery requests pass these redirect URLs explicitly, so neither flow depends on a localhost server or a public website domain.

Branded confirmation and recovery templates live in `supabase/email-templates/`. New Free projects cannot customize Auth email templates while using Supabase's default SMTP (platform change effective June 3, 2026). Once a domain and custom SMTP sender are configured, apply these subjects and bodies in Authentication > Emails:

- `Bestätige deine E-Mail-Adresse` with `confirm-sign-up.html`
- `Setze dein YOBRO-Passwort zurück` with `reset-password.html`

Keep `{{ .ConfirmationURL }}` unchanged. Disable click tracking in the chosen SMTP provider because rewritten Auth links may stop working.
