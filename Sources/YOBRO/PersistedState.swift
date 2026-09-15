import Foundation

/// Loading of YOBRO's JSON state files.
///
/// The previous `try? Data(...)` plus `try? decode(...)` pattern treated an
/// unreadable file exactly like a missing one: the browser started with an empty
/// session and the next autosave overwrote the original file. Tabs, spaces,
/// folders, history or bookmarks were then gone for good, without a word.
///
/// A file that exists but cannot be decoded is therefore moved aside and
/// reported, so the empty state is never silently written back over it.
enum PersistedState {
    struct Outcome<T> {
        var value: T?
        /// A message for the user, or `nil` when the file was fine or absent.
        var problem: String?
    }

    static func load<T: Decodable>(_ type: T.Type, at url: URL, decoder: JSONDecoder = JSONDecoder()) -> Outcome<T> {
        guard FileManager.default.fileExists(atPath: url.path) else { return Outcome(value: nil, problem: nil) }
        do {
            return Outcome(value: try decoder.decode(type, from: try Data(contentsOf: url)), problem: nil)
        } catch {
            return Outcome(value: nil, problem: setAside(url, reason: error))
        }
    }

    /// Renames the damaged file so the user can still recover it by hand.
    private static func setAside(_ url: URL, reason: Error) -> String {
        let name = url.lastPathComponent
        var backup = url.appendingPathExtension("corrupt")
        var attempt = 2
        while FileManager.default.fileExists(atPath: backup.path), attempt < 100 {
            backup = url.appendingPathExtension("corrupt-\(attempt)")
            attempt += 1
        }
        do {
            try FileManager.default.moveItem(at: url, to: backup)
            return L("„\(name)“ war beschädigt und liegt jetzt als „\(backup.lastPathComponent)“ daneben. YoBro startet mit einem leeren Stand, überschreibt die alte Datei aber nicht.",
                     "“\(name)” was damaged and is now kept alongside it as “\(backup.lastPathComponent)”. YoBro starts with an empty state but does not overwrite the old file.")
        } catch {
            return L("„\(name)“ konnte nicht gelesen werden: \(reason.localizedDescription)",
                     "“\(name)” could not be read: \(reason.localizedDescription)")
        }
    }
}
