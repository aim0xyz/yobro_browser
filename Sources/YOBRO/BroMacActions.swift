import AppKit
import Foundation

enum BroMacActions {
    /// Fixed allowlist: tool name → (display name, bundle identifier).
    static let apps: [String: (name: String, bundleID: String)] = [
        "spotify": ("Spotify", "com.spotify.client"), "discord": ("Discord", "com.hnc.Discord"),
        "chatgpt": ("ChatGPT", "com.openai.chat"), "claude": ("Claude", "com.anthropic.claudefordesktop"),
        "chrome": ("Google Chrome", "com.google.Chrome"), "safari": ("Safari", "com.apple.Safari"),
        "finder": ("Finder", "com.apple.finder"), "notes": ("Notes", "com.apple.Notes"),
        "messages": ("Messages", "com.apple.MobileSMS")
    ]

    /// Short, user-facing description shown when an action needs approval.
    static func summary(_ tool: String, _ args: [String: Any]) -> String? {
        switch tool {
        case "open_app":
            guard let key = args["app"] as? String, let app = apps[key.lowercased()] else { return nil }
            return L("\(app.name) öffnen", "Open \(app.name)")
        case "set_mac_volume":
            guard let level = args["level"] as? Int else { return nil }
            return L("Mac-Lautstärke auf \(level) % setzen", "Set Mac volume to \(level)%")
        case "spotify_control":
            guard let action = args["action"] as? String else { return nil }
            return L("Spotify: \(action)", "Spotify: \(action)")
        case "set_dark_mode":
            guard let enabled = args["enabled"] as? Bool else { return nil }
            return enabled ? L("macOS-Dunkelmodus einschalten", "Turn on macOS dark mode") : L("macOS-Dunkelmodus ausschalten", "Turn off macOS dark mode")
        default:
            return nil
        }
    }

    private static func appleScript(_ script: String) async throws -> String {
        try await Task.detached(priority: .userInitiated) {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", script]
            let output = Pipe(), errors = Pipe()
            process.standardOutput = output
            process.standardError = errors
            try process.run()
            let result = output.fileHandleForReading.readDataToEndOfFile()
            let problem = errors.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else {
                throw YOBROError.message(String(decoding: problem, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
            }
            return String(decoding: result, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        }.value
    }

    static func openApp(_ name: String) async throws -> String {
        guard let app = apps[name.lowercased()] else { throw YOBROError.message("Unknown app") }
        // Launch Services needs no Apple Events permission, unlike `tell … to activate`.
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: app.bundleID) else {
            throw YOBROError.message("\(app.name) is not installed on this Mac.")
        }
        _ = try await NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
        return "Opened \(app.name)."
    }

    static func setVolume(_ level: Int) async throws -> String {
        guard (0...100).contains(level) else { throw YOBROError.message("Volume must be between 0 and 100") }
        _ = try await appleScript("set volume output volume \(level)")
        return "Mac volume set to \(level)%."
    }

    static func spotify(_ action: String) async throws -> String {
        let command: String
        switch action {
        case "play": command = "play"
        case "pause": command = "pause"
        case "next": command = "next track"
        case "previous": command = "previous track"
        default: throw YOBROError.message("Unknown media action")
        }
        _ = try await appleScript("tell application \"Spotify\" to \(command)")
        return "Spotify: \(action)."
    }

    static func setDarkMode(_ enabled: Bool) async throws -> String {
        _ = try await appleScript("tell application \"System Events\" to tell appearance preferences to set dark mode to \(enabled ? "true" : "false")")
        return enabled ? "Dark mode enabled." : "Dark mode disabled."
    }
}
