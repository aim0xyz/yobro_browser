# YOBRO Mail 0.5

The sidebar envelope opens the native mail workspace, not Gmail or any webmail site. Multiple accounts feed a unified inbox; select an account, search loaded subject/sender fields, filter unread messages, read HTML or text, download attachments, compose or reply. Passwords are entered only in YOBRO's account editor and stored in macOS Keychain. Server metadata is stored locally in `mail-accounts.json`; message contents remain in memory for this version.

## Connecting accounts

Enter your email address. Known Gmail, iCloud and Microsoft domains are recognized locally. For other domains, YOBRO checks HTTPS autoconfig endpoints on the domain, then Thunderbird's public configuration database. Only the domain is transmitted, not the full email address or password. Discovered server names remain visible and editable before you press **Prüfen & verbinden**. This button verifies IMAP and SMTP authentication without sending mail and then saves the account.

- **Gmail:** preset for IMAP/SMTP with a Google app password, if the account permits it. Normal account passwords are not supported. OAuth-dependent Google accounts are not yet connectable.
- **iCloud Mail:** preset for IMAP and SMTP STARTTLS with an Apple app-specific password. Apple Mail is a client, not a separate mailbox provider.
- **Spacemail:** explicit preset for `mail.spacemail.com`, including custom-domain addresses. Automatic detection of an arbitrary custom domain may not reveal Spacemail; select the preset if needed.
- **Other providers:** discovered or manually configured IMAP over implicit TLS, SMTP over implicit TLS or mandatory STARTTLS. Separate SMTP username is supported; separate incoming/outgoing passwords are not yet supported.
- **Outlook / Microsoft 365:** recognized as requiring OAuth. YOBRO has not registered Microsoft OAuth clients, so these accounts are shown as unavailable in this preview. No false password fallback is offered as a working login method.

An app password is a credential used with IMAP/SMTP, not a different mail protocol. Provider detection cannot reliably determine whether a particular account's administrator has disabled app passwords, whether 2FA is active, or whether mailbox access is included in its plan. Actual connection testing is still necessary.

## Transport and behavior

The SwiftUI app runs a local Python 3 standard-library helper (`imaplib`, `smtplib`, `email`, `ssl`). This preview requires Python 3 at `/usr/bin/python3`, `/opt/homebrew/bin/python3`, or `/usr/local/bin/python3`; it has no pip dependencies. Python 3.9+ is required. The current Mac has a compatible runtime, but a redistributable embedded runtime or native transport is future packaging work.

Secrets are passed over the child process's stdin, never through command-line arguments or saved helper input files. TLS hostname and certificate validation are enabled. No unencrypted login or TLS bypass is implemented. Mail is connected directly to the configured provider, with no YOBRO backend.

YOBRO lists every IMAP folder reported by the provider, including nested paths and nonselectable parents. Selecting an account expands its folders. The newest 100 messages are loaded per selected folder, with a button to load older messages up to 5,000. Message identity includes account, folder, UIDVALIDITY and UID. Header and attachment fetches use BODY.PEEK; opening a successfully fetched message writes the server's Seen flag and updates the colored unread dots. A failed server update remains visible as unread.

The message list has a selection mode for selecting one, several, or all currently filtered messages. Selected messages can be moved to another folder of the current account or deleted as one server-side IMAP operation. Delete moves messages to the provider's `\\Trash` special-use folder (with localized-name fallback); only deleting while already inside that folder expunges them permanently. If the provider exposes no recognizable Trash folder, YOBRO refuses the operation instead of deleting permanently without warning.

HTML is rendered in a separate, nonpersistent WebKit view with JavaScript disabled and a restrictive content security policy. Inline MIME images referenced by Content-ID or Content-Location are supported. Remote images stay blocked until the user enables them for that message; links open in an YOBRO browser tab. The viewer overrides message-level scroll locks and resolves protocol-relative image URLs. A text toggle remains available. MIME attachments have filenames, sizes and a Save dialog. Empty plain-text alternatives fall back to HTML; decoding tolerates unknown charset declarations. Whole messages up to 64 MB can be fetched, with bounded HTML/text previews. Larger messages show an explicit error.

Accounts refresh every 60 seconds while YOBRO runs. The bell in Mail toggles native macOS notifications; enabling requests macOS permission. The first inbox sync establishes a baseline without flooding notifications. Subsequent unread UIDs trigger alerts; loading older messages does not. Clicking an alert opens its account. Notifications require YOBRO to remain running and are not IMAP push/IDLE. The unread dot covers folders reported by the server.

No full offline cache, folder creation/deletion, archive actions, attachment sending, OAuth token storage, or cross-platform synchronization yet. Encrypted S/MIME/PGP message bodies require external decryption.

Sending happens only when the user presses **Senden** in the native composer. Connection checking does not send messages. Successful SMTP acceptance is reported accurately; no automatic retries follow an uncertain result. Partial recipient refusal is reported. After SMTP acceptance, YOBRO appends a copy to the IMAP folder marked with the `\\Sent` special-use flag (or a recognized localized Sent folder). Gmail's SMTP service stores its own copy, so YOBRO does not append a duplicate there. A failed Sent-copy operation is reported separately and never changes a successful delivery into an ambiguous send failure. Draft text lives in memory until YOBRO quits. Replies use the original account and Reply-To plus In-Reply-To.

The browser's agent socket can open `panel mail` but does not expose mailbox credentials, message reads or sending. No real accounts were connected or real messages sent during development.

## Verification

`swift test` covers provider presets, OAuth-only detection, invalid addresses, XML authentication alternatives, password-free account metadata, and a real Swift-to-Python helper failure path that must not echo secrets.

`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_mail.py -v` covers MIME decoding, attachments, HTML-to-text, header injection, certificate verification, STARTTLS-before-auth, UID/PEEK reads, stale UIDVALIDITY and size rejection, composer serialization and a non-sending connection check. Mail server behavior is mocked in these tests; these are not proof of live provider interoperability. Actual account connection and delivery require a user-configured account.

Provider references:
- https://support.google.com/mail/answer/7126229
- https://support.apple.com/en-us/102525
- https://www.spaceship.com/en-GB/knowledgebase/connect-spacemail-to-email-client/
- https://github.com/thunderbird/autoconfig
- https://support.microsoft.com/en-us/support/known-issues/modern-authentication-methods-now-needed-to-continue-syncing-outlook-email-in-non-microsoft-email-ap

## Repeated macOS Keychain dialogs

YOBRO caches each authorized mail credential in process memory for the app session. Background refreshes do not repeatedly read the same Keychain entry. Denied reads are also remembered; explicitly pressing Mail Refresh retries them. Editing credentials replaces the cache and disconnecting clears it. No plaintext credential file is created.

The macOS dialog refers to the login Keychain password. Always Allow persists an access decision; Allow covers the current access. Repeated ad-hoc builds can change the app's signing requirement and require a fresh decision. Use the same Developer ID certificate for successive builds. `YOBRO_SIGN_IDENTITY='Developer ID Application: …' ./scripts/build.sh` now supports this without requiring a passkey provisioning profile. The Developer ID identity was found after checking outside the sandbox. This workspace now selects it from `.yobro-signing-identity` for subsequent builds. Existing Keychain access decisions may need one final approval when moving from the old ad-hoc build.

In 0.6.3 automatic Keychain reads explicitly disable authentication UI for the duration of the synchronous legacy-Keychain operation and restore the previous policy immediately. If authorization is required, Mail lists the affected account with a **Mail entsperren** button. Only this explicit action permits an interactive read. This avoids startup/polling prompts while retaining automatic sync for already authorized accounts.
