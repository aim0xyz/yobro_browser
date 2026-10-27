# YoBro

**Your browser. Your bro.**

A native macOS browser built with SwiftUI, AppKit and WebKit. Spaces with their own colour, a sidebar for tabs and folders, your IMAP mailbox beside your tabs, and an assistant that runs on your own API key.

Website: [yobro.lol](https://yobro.lol) · Download: [latest release](https://github.com/aim0xyz/yobro_browser/releases/latest) (Apple Silicon, macOS 14 or later)

## Build

Requires macOS 14 or later and the Xcode command-line tools.

```sh
./scripts/build.sh
open dist/YoBro.app
```

`./scripts/build-dmg.sh --local` builds an unsigned test DMG. Public releases are signed with a Developer ID and notarized by `.github/workflows/release.yml`.

## Privacy

YoBro has no server, account or sync. Profiles, tabs, history, bookmarks and website data stay on the Mac. Passwords, API keys and mail credentials live in the macOS Keychain. The assistant talks to the provider you configure: OpenRouter or any OpenAI-compatible endpoint, including a local server. Details: [yobro.lol/en/privacy](https://yobro.lol/en/privacy/).

## External agents

In Settings, you can let Claude Desktop or Codex control YoBro over a local Unix socket. Access is off until you allow it, and you can pause it at any time. The CLI in `bin/yobro` speaks the same protocol:

```sh
./bin/yobro status
./bin/yobro new https://example.com
./bin/yobro read
```
