import AppKit
import Darwin
import Foundation

struct BroWorkspace: Codable, Identifiable, Equatable {
    let id: UUID
    let name: String
    let path: String
    let bookmark: Data?

    init(url: URL) {
        id = UUID()
        name = url.lastPathComponent
        path = url.resolvingSymlinksInPath().standardizedFileURL.path
        bookmark = try? url.bookmarkData(options: .withSecurityScope)
    }
}

struct BroASCCommand: Equatable {
    let arguments: [String]
    let requiresApproval: Bool

    init(_ arguments: [String]) throws {
        guard let verb = arguments.first, arguments.count <= 6,
              arguments.allSatisfy({ !$0.isEmpty && $0.count <= 160 && $0.rangeOfCharacter(from: .controlCharacters) == nil }) else {
            throw YOBROError.message("Invalid App Store HQ command")
        }
        switch verb {
        case "status", "apps":
            guard arguments.count == 1 || arguments == [verb, "--json"] else { throw YOBROError.message("Unsupported read command") }
            requiresApproval = false
        case "app", "metadata", "locales", "screenshots":
            guard arguments.count >= 2, (arguments.count == 2 || arguments == [verb, arguments[1], "--json"]),
                  !arguments[1].hasPrefix("-") else { throw YOBROError.message("Unsupported read command") }
            requiresApproval = false
        case "new-version":
            guard arguments.count == 6, !arguments[1].hasPrefix("-"), arguments[2] == "--version",
                  arguments[3].range(of: #"^[0-9]+\.[0-9]+(?:\.[0-9]+)?$"#, options: .regularExpression) != nil,
                  arguments[4] == "--platform", ["IOS", "MAC_OS", "TV_OS"].contains(arguments[5]) else {
                throw YOBROError.message("Use new-version APP --version 1.2.3 --platform IOS|MAC_OS|TV_OS")
            }
            requiresApproval = true
        default:
            throw YOBROError.message("This App Store HQ command is not enabled in YoBro")
        }
        self.arguments = arguments
    }
}

struct BroCommandApproval: Equatable {
    let workspaceID: UUID
    let workspace: String
    let arguments: [String]
}

@MainActor
final class BroWorkspaceStore: ObservableObject {
    @Published private(set) var folders: [BroWorkspace] = []
    @Published var error: String?
    private let file: URL

    init(home: URL) {
        file = home.appendingPathComponent("bro-workspaces.json")
        if let data = try? Data(contentsOf: file) {
            do { folders = try JSONDecoder().decode([BroWorkspace].self, from: data) }
            catch { self.error = error.localizedDescription }
        }
    }

    func add(_ url: URL) throws {
        let root = url.resolvingSymlinksInPath().standardizedFileURL
        var directory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: root.path, isDirectory: &directory), directory.boolValue else {
            throw YOBROError.message(L("Bitte einen vorhandenen Ordner wählen.", "Choose an existing folder."))
        }
        guard !folders.contains(where: { $0.path == root.path }) else { return }
        let next = folders + [BroWorkspace(url: url)]
        try persist(next)
        folders = next
    }

    func remove(_ id: UUID) throws {
        let next = folders.filter { $0.id != id }
        try persist(next)
        folders = next
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = L("Ordner als Arbeitsmaterial für Bro freigeben", "Allow Bro to use this folder as working material")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try add(url); error = nil }
        catch { self.error = error.localizedDescription }
    }

    private func persist(_ folders: [BroWorkspace]) throws {
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(folders).write(to: file, options: [.atomic, .completeFileProtection])
    }

    func workspace(_ id: UUID) throws -> BroWorkspace {
        guard let folder = folders.first(where: { $0.id == id }) else { throw YOBROError.message("Workspace not found") }
        return folder
    }

    private func root(_ id: UUID) throws -> URL {
        let folder = try workspace(id)
        var stale = false
        let bookmarked = try? folder.bookmark.flatMap {
            try URL(resolvingBookmarkData: $0, options: .withSecurityScope, bookmarkDataIsStale: &stale)
        }
        let root = (bookmarked ?? URL(fileURLWithPath: folder.path)).resolvingSymlinksInPath().standardizedFileURL
        guard root.path == folder.path, FileManager.default.fileExists(atPath: root.path) else {
            throw YOBROError.message("Workspace is unavailable or moved: \(folder.name)")
        }
        return root
    }

    private func child(_ relative: String, in root: URL) throws -> URL {
        guard !relative.hasPrefix("/"), !relative.components(separatedBy: "/").contains("..") else {
            throw YOBROError.message("Path must stay inside the workspace")
        }
        let file = root.appendingPathComponent(relative).resolvingSymlinksInPath().standardizedFileURL
        guard file.path == root.path || file.path.hasPrefix(root.path + "/") else {
            throw YOBROError.message("Path must stay inside the workspace")
        }
        let parts = file.path.dropFirst(root.path.count).split(separator: "/").map(String.init)
        guard !parts.contains(where: Self.protectedPart) else {
            throw YOBROError.message("Protected file is unavailable to Bro")
        }
        return file
    }

    private static func protectedPart(_ part: String) -> Bool {
        let value = part.lowercased()
        if value.hasPrefix(".") || ["node_modules", "dist", "build"].contains(value) { return true }
        let stem = (value as NSString).deletingPathExtension
        let words = stem.split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init)
        let sensitive = Set(["auth", "authentication", "credential", "credentials", "secret", "secrets",
                             "token", "tokens", "password", "passwords", "private", "keychain", "keystore"])
        return words.contains(where: sensitive.contains)
            || ["id_rsa", "id_ed25519", "asc.config.json"].contains(value)
            || ["p8", "pem", "key", "p12", "pfx"].contains((value as NSString).pathExtension)
    }

    func list(_ id: UUID, path: String = "") throws -> String {
        let root = try root(id)
        let accessing = root.startAccessingSecurityScopedResource()
        defer { if accessing { root.stopAccessingSecurityScopedResource() } }
        let directory = try child(path, in: root)
        let names = try FileManager.default.contentsOfDirectory(atPath: directory.path)
            .filter { !Self.protectedPart($0) }
            .sorted().prefix(100)
        let rows = names.map { name -> [String: Any] in
            let url = directory.appendingPathComponent(name)
            var directoryFlag: ObjCBool = false
            _ = FileManager.default.fileExists(atPath: url.path, isDirectory: &directoryFlag)
            return ["name": name, "directory": directoryFlag.boolValue]
        }
        return String(decoding: try JSONSerialization.data(withJSONObject: ["path": path, "entries": rows]), as: UTF8.self)
    }

    func read(_ id: UUID, path: String) throws -> String {
        let root = try root(id)
        let accessing = root.startAccessingSecurityScopedResource()
        defer { if accessing { root.stopAccessingSecurityScopedResource() } }
        let url = try child(path, in: root)
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
        guard values.isRegularFile == true, (values.fileSize ?? 0) <= 256_000 else {
            throw YOBROError.message("Only text files up to 256 KB can be read")
        }
        let data = try Data(contentsOf: url)
        guard let contents = String(data: data, encoding: .utf8), !contents.contains("\0") else {
            throw YOBROError.message("Only UTF-8 text files can be read")
        }
        return String(contents.prefix(30_000))
    }

    func runASC(_ id: UUID, command: BroASCCommand) async throws -> String {
        let folder = try workspace(id)
        let expected = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Documents/appstore-hq").resolvingSymlinksInPath().standardizedFileURL
        guard folder.path == expected.path else {
            throw YOBROError.message("App Store HQ commands are available only for the registered Documents/appstore-hq folder")
        }
        let root = try root(id)
        let accessing = root.startAccessingSecurityScopedResource()
        defer { if accessing { root.stopAccessingSecurityScopedResource() } }
        let packageURL = root.appendingPathComponent("package.json")
        guard let packageData = try? Data(contentsOf: packageURL),
              let package = try JSONSerialization.jsonObject(with: packageData) as? [String: Any],
              let scripts = package["scripts"] as? [String: String], scripts["asc"] == "tsx cli/index.ts" else {
            throw YOBROError.message("The expected App Store HQ CLI was not found")
        }
        let npm = ["/opt/homebrew/bin/npm", "/usr/local/bin/npm"].first { FileManager.default.isExecutableFile(atPath: $0) }
        guard let npm else { throw YOBROError.message("npm is not installed at a supported location") }
        let process = Process()
        let worker = Task.detached(priority: .userInitiated) {
            try Task.checkCancellation()
            process.executableURL = URL(fileURLWithPath: npm)
            // --ignore-scripts keeps npm from running `preasc`/`postasc` hooks;
            // only the verified `asc` script itself runs.
            process.arguments = ["--prefix", root.path, "--ignore-scripts", "run", "asc", "--"] + command.arguments
            process.currentDirectoryURL = root
            var environment = ProcessInfo.processInfo.environment
            environment["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:" + (environment["PATH"] ?? "")
            process.environment = environment
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            try process.run()
            if Task.isCancelled { Self.terminateTree(process.processIdentifier); throw CancellationError() }
            // npm starts tsx and node as children. They hold the pipe open, so
            // the timeout must end the whole tree, not only npm.
            let timeout = DispatchWorkItem { if process.isRunning { Self.terminateTree(process.processIdentifier) } }
            DispatchQueue.global().asyncAfter(deadline: .now() + 120, execute: timeout)
            let output = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            timeout.cancel()
            try Task.checkCancellation()
            let text = String(decoding: output.prefix(40_000), as: UTF8.self)
            guard process.terminationStatus == 0 else { throw YOBROError.message("App Store HQ exited \(process.terminationStatus): \(text)") }
            return text
        }
        return try await withTaskCancellationHandler {
            try await worker.value
        } onCancel: {
            worker.cancel()
            if process.isRunning { Self.terminateTree(process.processIdentifier) }
        }
    }

    /// Ends a process and all of its descendants, children first.
    nonisolated static func terminateTree(_ pid: pid_t) {
        func descendants(of parent: pid_t) -> [pid_t] {
            var children = [pid_t](repeating: 0, count: 256)
            let count = proc_listchildpids(parent, &children, Int32(children.count * MemoryLayout<pid_t>.size))
            guard count > 0 else { return [] }
            return children.prefix(Int(count)).flatMap { descendants(of: $0) + [$0] }
        }
        let tree = descendants(of: pid) + [pid]
        for member in tree { kill(member, SIGTERM) }
        DispatchQueue.global().asyncAfter(deadline: .now() + 3) {
            for member in tree where kill(member, 0) == 0 { kill(member, SIGKILL) }
        }
    }
}
