# YoBro Mail

The envelope next to the assistant (in the sidebar footer, or in the strip with tabs on top) and **Browser → Mail** (⇧⌘M) open YoBro's native mail client in place of the page. It is not a webmail site. Opening or selecting a tab closes it again. Multiple accounts feed a unified inbox; select an account, search loaded subject/sender fields, filter unread messages, read HTML or text, download attachments, compose, reply or work on a message with the assistant. Passwords are entered only in YoBro's account editor and stored in the macOS Keychain. Server metadata is stored locally in `mail-accounts.json`; message contents remain in memory.

## Connecting accounts

Enter your email address. Known Gmail, iCloud and Microsoft domains are recognized locally (Microsoft in every country, such as `outlook.de` or `hotmail.co.uk`). For other domains, YoBro checks HTTPS autoconfig endpoints on the domain, then Thunderbird's public configuration database. Only the domain is transmitted, not the full email address or password. Discovered server names remain visible and editable before you press **Verbinden**. This verifies IMAP and SMTP sign-in without sending mail and then saves the account. When editing an account, an empty password field keeps the saved password.

- **Gmail:** IMAP/SMTP with a Google app password, if the account permits it. OAuth-only Google accounts are not yet connectable.
- **iCloud Mail:** IMAP and SMTP STARTTLS with an Apple app-specific password.
- **Spacemail:** explicit preset for `mail.spacemail.com`, including custom-domain addresses.
- **Other providers:** discovered or manually configured IMAP over implicit TLS, SMTP over implicit TLS or mandatory STARTTLS. A separate SMTP username is supported.
- **Outlook / Microsoft 365:** recognized as requiring OAuth. YoBro has no registered Microsoft OAuth client, so these accounts are shown as unavailable.

Passwords may contain any characters. ASCII passwords use `LOGIN`; others use SASL `PLAIN` in UTF-8, which `imaplib` and `smtplib` cannot do with `LOGIN`.

## Transport and behavior

The app runs a local Python 3 standard-library helper (`Resources/MailWorker.py`: `imaplib`, `smtplib`, `email`, `ssl`) from `/usr/bin/python3`, `/opt/homebrew/bin/python3` or `/usr/local/bin/python3`; Python 3.9+ is required and there are no pip dependencies. Secrets are passed over the child process's stdin, never through arguments or files. TLS hostname and certificate validation are always on. Mail goes straight to the configured provider, with no YoBro backend.

Errors are told apart: a rejected password reads "Sign-in failed", while a command the server refuses after sign-in reads "The mail server refused the action". Server transcripts are never shown, since they can echo credentials.

YoBro lists every IMAP folder, including nested paths and nonselectable parents. The newest 100 messages load per folder, with **Load older messages** up to 5,000. Message identity includes account, folder, UIDVALIDITY and UID. A message with a damaged header is listed with a placeholder subject instead of hiding the folder; one without a usable `Date` shows when the server received it. Header and attachment fetches use `BODY.PEEK`; opening a message sets the server's Seen flag, and **Mark as unread** clears it.

Messages can be moved or deleted one at a time from the reader or the list's context menu, or several at once in selection mode. Delete moves messages to the provider's `\Trash` special-use folder (with localized-name fallback); only deleting inside that folder expunges them. Without a recognizable Trash folder nothing is deleted. Moving uses `UID MOVE` when the server announces `MOVE`; otherwise `COPY`, `\Deleted` and, only with `UIDPLUS`, `UID EXPUNGE` of exactly those messages. Without `UIDPLUS`, the originals stay flagged and hidden rather than expunging other clients' deleted messages.

HTML is rendered in a separate, nonpersistent WebKit view with JavaScript disabled and a restrictive content security policy; it reloads only when the message or its image permission changes. Inline MIME images referenced by Content-ID (in double or single quotes, or CSS `url(...)`) or by Content-Location in image attributes are supported. Images from other servers stay blocked: a bar in the message offers **Load** for this message or **Always from this sender**. The ··· menu can load them everywhere and forget trusted senders. Links open in a YoBro tab. A plain-text toggle remains available. MIME attachments have filenames, sizes and a Save dialog. Whole messages up to 64 MB can be fetched.

Accounts refresh every 5 minutes while YoBro runs, for the profile that is open. The ··· menu toggles native macOS notifications; enabling requests permission. Only the newest inbox UID per account is remembered: the first refresh sets that baseline, and later unread messages above it trigger an alert. Clicking an alert opens Mail on its account.

Sending happens only when the user presses **Senden** (⌘↩). Successful SMTP acceptance is reported accurately, with no automatic retries after an uncertain result. Partial recipient refusal is reported. After acceptance, YoBro appends a copy to the `\Sent` folder (Gmail stores its own). A failed Sent copy is reported separately. Replies go to Reply-To (or the sender), quote the original below an empty first line and carry `In-Reply-To` and `References`. Cc is available. The draft lives in memory until YoBro quits.

The sparkles menu in a message puts a request and the message text into the assistant's composer, with a note that the email is untrusted content. Nothing is sent to the model until you send it there.

## Verification

`swift test --filter Mail` covers provider presets and Microsoft domains, OAuth-only detection, invalid addresses, XML authentication alternatives, password-free account metadata, the inbox baseline and its migration, per-message and per-sender images, replies, Cc and threading, marking unread, editing without a password, removal that keeps the password when the list cannot be saved, polling that releases a closed profile's store, rich-text colors, and a real Swift-to-Python failure path that must not echo secrets.

`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p test_mail.py -v` covers MIME decoding, attachments, inline images, HTML-to-text, header injection, certificate verification, STARTTLS before sign-in, UTF-8 passwords, sign-in versus refused-command errors, UID/PEEK reads, damaged headers, missing dates, stale UIDVALIDITY and size limits, move and delete with and without `MOVE`/`UIDPLUS`, composer serialization and a non-sending connection check. Mail servers are mocked; these tests are not proof of live provider interoperability.

## Repeated macOS Keychain dialogs

YoBro caches each authorized mail credential in process memory for the app session. Background refreshes do not repeatedly read the same Keychain entry; denied reads are remembered, and pressing Refresh retries them. Editing credentials replaces the cache and removing an account clears it.

Automatic Keychain reads disable authentication UI for the duration of the synchronous legacy-Keychain operation and restore the previous policy immediately. If authorization is required, Mail lists the account with an **Unlock mail** button; only this explicit action permits an interactive read. Repeated ad-hoc builds can change the app's signing requirement and require a fresh decision; signing successive builds with the same Developer ID (`YOBRO_SIGN_IDENTITY`, or `.yobro-signing-identity`) avoids that.
