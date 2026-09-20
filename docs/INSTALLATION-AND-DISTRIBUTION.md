# Installation and distribution

The macOS download is a DMG with `YoBro.app`, an Applications shortcut and bilingual
instructions. Drag the app into Applications, eject the DMG and start the app.
No privileged installer or background service is required for local agent access.

The onboarding flow offers optional browser import, optional agent setup, then completion.
Settings → Agents exposes the same setup after onboarding. Agent access starts **off**,
is enabled explicitly and persists per profile; pausing also persists. Switching away
from a profile pauses it. History/download-list access remains a separate opt-in.

## Local agents

- The app contains a native `yobro-mcp` executable. End users need no Python or Node
  installation for this adapter. Mail transport/import features still have their
  separately documented Python dependency.
- Claude: save the bundled `YOBRO.mcpb` from Settings → Agents; install it in Claude
  Settings → Extensions. The user confirms that installation in Claude.
- Codex: Settings → Agents invokes the installed Codex CLI with structured arguments
  to register the bundled `yobro-bundled` marketplace and install `yobro-browser`.
  The integration includes the browser workflow skill as well as MCP tools. Start a new
  agent conversation after installation. App/organization plugin policies still apply.
- Setup is blocked when running directly from a disk image or App Translocation.
- Binding files contain only a socket basename in
  `~/Library/Application Support/YOBRO/AgentConnections/<client>.json` (0600, directory
  0700). No browser profile data or credentials are copied into plugins.
- Bindings remain attached to the chosen profile; there is no automatic fallback to
  the original or currently active profile. Unbound/missing/inactive/paused profiles
  produce actionable errors. Rebind explicitly from the intended profile's settings.
- Removing a connection deletes its binding and pauses browser access. Uninstall the
  plugin in the agent app as well if it is no longer needed.
- The connection check validates the local adapter-to-browser path only. A status call
  from Claude/Codex is still required to confirm agent installation end to end.
- Existing developer-only `bin/yobro` and Python plugin remain available for explicit
  socket workflows; the packaged integrations use the new native adapter.

This is a same-user local interface, not cryptographic per-app authentication: other
processes running as the same macOS user can reach an enabled browser socket. Its
filesystem permissions and peer UID are checked. No TCP listener is introduced.
Page content requested by a cloud agent is sent to that agent provider. Profile
cookies/login sessions are used by WebKit; cookie databases and Keychain entries are
not exported by the installer.

## ChatGPT remote

The app clearly marks this connection as unavailable. No remote tunnel, public socket,
or account connection is silently created. A production remote integration still
requires an authenticated HTTPS MCP service, user/device pairing, revocation, and
deployment. OpenAI Secure MCP Tunnel is an option for private development; it does not
replace the public endpoint required for public plugin distribution.

## VPN privacy

The previous built-in VPN location/IP has been removed. VPN keys were read from
macOS Keychain, not hardcoded. The environment-driven bootstrap/FIFO path and its
shared temporary status file have also been removed.

New installs have zero VPN locations. Users can enter their own Outline credentials
under Settings → Proxy / VPN. Location metadata is stored in the profile-local
`managed-vpn-locations.json` (0600); the access key goes only into Keychain and is
passed to the transport helper through stdin, not process arguments.

An existing selected VPN with no corresponding local location configuration stays
blocked until reconfigured or explicitly switched off. It never silently falls back
to direct browsing. Existing Keychain items are not read, exported or deleted by the
packaging workflow. A formerly built-in location must be configured again by its owner.

The public Supabase project URL and publishable key remain client configuration, not
secret credentials. User tokens remain separate. Backend authorization/RLS is outside
the distribution scan; a public key must never be treated as authorization by itself.

## Build

```sh
./scripts/build-dmg.sh --local
```

Produces `dist/YoBro-<version>-<architecture>-local.dmg`, its SHA-256 checksum,
`dist/YoBro.app`, and `dist/privacy-audit.json`. Local builds are not notarized public
releases. Current build architecture follows the build Mac; do not advertise an
Apple Silicon build as Intel-compatible. The bundled VPN helper must also match the
advertised architecture.

```sh
YOBRO_SIGN_IDENTITY='Developer ID Application: …' \
YOBRO_NOTARY_PROFILE='your-stored-notary-profile' \
./scripts/build-dmg.sh --release
```

Only a Keychain profile **name**, never an Apple password or API private key, is passed
to `notarytool`. The public release path requires these settings, signs, notarizes and
staples the app and disk image, assesses Gatekeeper, verifies the image and writes the
checksum. The public artifact is created only after these checks pass. Certificates,
Apple notarization access and remote hosting are not included in the repository.

## Packaging guard and tests

Builds copy an explicit allowlist of resources and integration files. They never
copy Application Support, browser profiles, Keychain databases, dot-env files or
the repository wholesale. Compiler prefix maps remove personal checkout paths.

`scripts/audit-distribution.py` checks shipped bytes, filenames, and nested ZIP/MCPB
archives for recognizable private keys, provider tokens, VPN access keys, credential
URLs, JWTs, personal macOS build paths and private state files. It rejects unexpected
symlinks and unsafe archive members. Findings include only rule names/file locations,
never matched secrets. The report includes hashes of all top-level package files.
This is a pattern-based guard and explicit packaging boundary, **not a mathematical
guarantee that arbitrary code/binaries contain no sensitive data**.

```sh
swift test --filter 'DistributionSetupTests|OnboardingTests|AgentActivityTests|AgentWorkspaceTests'
python3 -m unittest discover -s tests -p test_distribution.py -v
python3 scripts/verify-dmg.py dist/YoBro-0.6.6-arm64-local.dmg
```

Native MCP tests use only synthetic short-lived Unix sockets. UI tests use temporary
profile directories. A fresh-Mac Gatekeeper check and actual Claude/Codex install
verification remain release acceptance checks, not claims made by these unit tests.
The DMG verification mounts read-only, copies the app to a different temporary path,
checks signatures and packaged data, extracts the Claude extension, and runs the
native adapter tests against all three delivered executable locations.

References: [Apple distribution](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution),
[Claude extensions](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop),
[MCPB manifest](https://github.com/anthropics/mcpb/blob/main/MANIFEST.md),
[OpenAI tunnels](https://developers.openai.com/api/docs/guides/secure-mcp-tunnels).


## Extensions (0.6.6)

The extension menu opens the native `yobro://extensions` hub, also available offline.
It is restored as an internal tab and is never requested from a website. The hub
includes uBlock Origin Lite controls (macOS 15.6+) and the built-in website dark
mode. Users can restore the bundled blocker after intentionally removing it.

Search and catalog links use the Chrome Web Store without a country storefront.
Availability of external providers can still vary by region. Only the exact HTTPS
Chrome Web Store host receives a catalog-compatible browser identity, avoiding
its Safari redirect to `/unsupported`; other sites retain the Safari identity. Safari App Store
apps install into Safari, not YoBro; YoBro accepts compatible WebExtension folders,
ZIPs and CRXs. The permission review remains mandatory; WebKit compatibility is
not guaranteed for arbitrary Chrome extensions. CRX publisher signatures are not
verified by this local package importer.
