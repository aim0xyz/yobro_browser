import SwiftUI
import AppKit

struct LibraryView: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var downloads: DownloadStore
    @State var section: LibrarySection
    @State private var query = ""
    @State private var confirmClear = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                YOBROMark(size: 24)
                Text(L("Deine Bibliothek")).font(.system(size: 22, weight: .medium, design: .serif))
                Spacer()
                Button { model.librarySection = nil } label: { Image(systemName: "xmark").padding(6) }.buttonStyle(YOBROButtonStyle()).keyboardShortcut(.cancelAction)
            }.padding(25)
            HStack(spacing: 4) {
                ForEach(LibrarySection.allCases) { value in
                    Button { section = value } label: {
                        Label(L(value.rawValue), systemImage: value.symbol)
                            .font(.system(size: 12, weight: .medium)).frame(maxWidth: .infinity).padding(11)
                            .background(section == value ? YOBROTheme.surface : .clear, in: RoundedRectangle(cornerRadius: 8))
                    }.buttonStyle(YOBROButtonStyle())
                }
            }.padding(4).background(moss.opacity(0.07), in: RoundedRectangle(cornerRadius: 11)).padding(.horizontal, 25).padding(.bottom, 20)
            switch section {
            case .history: historyView
            case .bookmarks: bookmarksView
            case .downloads: downloadsView
            }
        }.frame(width: 680, height: 570).background(paper).foregroundStyle(ink)
            .overlay {
                if confirmClear {
                    YOBRODialogOverlay(icon: "clock.arrow.circlepath", title: L("Verlauf löschen?"), message: L("Die Liste besuchter Seiten wird gelöscht. Offene Tabs und Logins bleiben erhalten."), confirmTitle: L("Verlauf löschen"), cancelTitle: L("Abbrechen"), destructive: true, confirm: { confirmClear = false; model.clearHistory() }, cancel: { confirmClear = false })
                }
            }
    }
    private var historyView: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "magnifyingglass").foregroundStyle(moss)
                TextField(L("Im Verlauf suchen"), text: $query).textFieldStyle(.plain)
            }.font(.system(size: 13)).padding(13).background(YOBROTheme.surface, in: RoundedRectangle(cornerRadius: 10)).padding(.horizontal, 25).padding(.bottom, 14)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 5) {
                    if model.matchingHistory(query).isEmpty { empty(L("Noch nichts gefunden"), L("Hier findest du besuchte Seiten wieder."), "clock.arrow.circlepath") }
                    ForEach(model.matchingHistory(query)) { entry in
                        Button { model.newTab(url: entry.url); model.librarySection = nil } label: {
                            HStack(spacing: 13) {
                                Image(systemName: "globe").foregroundStyle(moss).frame(width: 34, height: 34).background(moss.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
                                VStack(alignment: .leading, spacing: 5) {
                                    Text(entry.title).font(.system(size: 12, weight: .medium)).lineLimit(1)
                                    Text(entry.url).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5)).lineLimit(1)
                                }
                                Spacer()
                                Text(entry.date, format: .dateTime.day().month().hour().minute()).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45))
                                Image(systemName: "arrow.up.right").font(.system(size: 10)).foregroundStyle(moss)
                            }.padding(12).background(YOBROTheme.surface.opacity(0.65), in: RoundedRectangle(cornerRadius: 10))
                        }.buttonStyle(YOBROButtonStyle())
                    }
                }.padding(.horizontal, 25)
            }
            HStack {
                Text(L("\(model.history.count) Seiten · Nur auf deinem Mac", "\(model.history.count) pages · Only on your Mac")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
                Spacer()
                Button(L("Verlauf löschen …")) { confirmClear = true }.font(.system(size: 11)).buttonStyle(YOBROButtonStyle()).disabled(model.history.isEmpty)
            }.padding(25)
        }
    }
    private var matchingBookmarks: [BookmarkEntry] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return model.bookmarks }
        return model.bookmarks.filter {
            $0.title.localizedCaseInsensitiveContains(needle)
                || $0.url.localizedCaseInsensitiveContains(needle)
                || $0.folder.localizedCaseInsensitiveContains(needle)
        }
    }

    private var bookmarksView: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "magnifyingglass").foregroundStyle(moss)
                TextField(L("Lesezeichen suchen"), text: $query).textFieldStyle(.plain)
            }.font(.system(size: 13)).padding(13).background(YOBROTheme.surface, in: RoundedRectangle(cornerRadius: 10)).padding(.horizontal, 25).padding(.bottom, 14)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 5) {
                    if matchingBookmarks.isEmpty {
                        empty(L("Noch keine Lesezeichen", "No bookmarks yet"),
                              L("Merke die aktuelle Seite mit ⌘D.", "Save the current page with ⌘D."),
                              "bookmark")
                    }
                    ForEach(matchingBookmarks) { entry in
                        HStack(spacing: 13) {
                            Button { model.newTab(url: entry.url); model.librarySection = nil } label: {
                                HStack(spacing: 13) {
                                    Image(systemName: "bookmark.fill").foregroundStyle(moss).frame(width: 34, height: 34).background(moss.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
                                    VStack(alignment: .leading, spacing: 5) {
                                        Text(entry.title).font(.system(size: 12, weight: .medium)).lineLimit(1)
                                        Text(entry.url).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5)).lineLimit(1)
                                    }
                                    Spacer()
                                    Text(entry.folder).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45)).lineLimit(1)
                                    Image(systemName: "arrow.up.right").font(.system(size: 10)).foregroundStyle(moss)
                                }
                            }.buttonStyle(.plain)
                            Button { model.removeBookmark(entry) } label: {
                                Image(systemName: "trash").font(.system(size: 11)).frame(width: 28, height: 28)
                            }.buttonStyle(YOBROButtonStyle(minimumSize: 28)).help(L("Lesezeichen entfernen"))
                        }.padding(12).background(YOBROTheme.surface.opacity(0.65), in: RoundedRectangle(cornerRadius: 10))
                    }
                }.padding(.horizontal, 25)
            }
            HStack {
                Text(L("\(model.bookmarks.count) Lesezeichen", "\(model.bookmarks.count) bookmarks")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
                Spacer()
                Button(L("Aktuelle Seite merken")) { model.bookmarkActivePage() }
                    .font(.system(size: 11)).buttonStyle(YOBROButtonStyle())
                    .disabled(model.active?.webView.url == nil)
            }.padding(25)
        }
    }

    private var downloadsView: some View {
        VStack(spacing: 0) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 10) {
                    if downloads.entries.isEmpty { empty(L("Alles an einem Ort"), L("Deine Downloads landen im Ordner Downloads/YOBRO."), "arrow.down.circle") }
                    ForEach(downloads.entries) { entry in
                        HStack(alignment: .top, spacing: 13) {
                            Image(systemName: entry.state == "completed" ? "doc.text" : "arrow.down.document").font(.system(size: 23)).foregroundStyle(moss).frame(width: 37).padding(.top, 4)
                            VStack(alignment: .leading, spacing: 8) {
                                Text(entry.name).font(.system(size: 13, weight: .medium)).lineLimit(1)
                                if entry.state == "downloading" {
                                    if entry.expected > 0 { ProgressView(value: entry.fraction).tint(moss) }
                                    else { ProgressView().controlSize(.small) }
                                }
                                HStack {
                                    Text(entry.stateLabel); Text("·")
                                    Text(ByteCountFormatter.string(fromByteCount: entry.received, countStyle: .file))
                                }.font(.system(size: 10)).foregroundStyle(entry.state == "failed" ? .red : ink.opacity(0.55))
                                if let error = entry.error { Text(error).font(.system(size: 10)).foregroundStyle(.red).lineLimit(2) }
                                Text(URL(string: entry.source)?.host ?? L("Webseite")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.4))
                            }
                            Spacer(minLength: 0)
                            if entry.state == "downloading" {
                                Button { downloads.cancel(entry.id) } label: { Image(systemName: "xmark.circle").padding(5) }.buttonStyle(YOBROButtonStyle()).help(L("Download abbrechen"))
                            } else if entry.state == "completed" {
                                Button { downloads.reveal(entry) } label: { Image(systemName: "folder").padding(5) }.buttonStyle(YOBROButtonStyle()).help(L("Im Finder zeigen"))
                            }
                        }.padding(16).background(YOBROTheme.surface.opacity(0.75), in: RoundedRectangle(cornerRadius: 12))
                    }
                }.padding(.horizontal, 25)
            }
            HStack {
                Text(L("\(downloads.activeCount) aktive Downloads", "\(downloads.activeCount) active downloads")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
                Spacer()
                Button(L("Downloadordner öffnen")) {
                    do { try FileManager.default.createDirectory(at: downloads.directory, withIntermediateDirectories: true); NSWorkspace.shared.open(downloads.directory) }
                    catch { model.notice = error.localizedDescription }
                }.font(.system(size: 11)).buttonStyle(YOBROButtonStyle())
            }.padding(25)
        }
    }
    private func empty(_ title: String, _ detail: String, _ icon: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: icon).font(.system(size: 32, weight: .light)).foregroundStyle(moss)
            Text(title).font(.system(size: 23, design: .serif))
            Text(detail).font(.system(size: 12)).foregroundStyle(ink.opacity(0.5))
        }.frame(maxWidth: .infinity).padding(.vertical, 72)
    }
}

struct PaletteResult: Identifiable {
    let id: String
    let title: String
    let subtitle: String
    let icon: String
    let tabID: UUID?
    let url: String?
}

struct CommandPalette: View {
    @ObservedObject var model: BrowserModel
    @State private var query = ""
    @State private var selected = 0
    @FocusState private var focused: Bool
    private var results: [PaletteResult] {
        let term = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let open = model.tabs.filter { term.isEmpty || $0.title.localizedCaseInsensitiveContains(term) || $0.url.localizedCaseInsensitiveContains(term) }
            .prefix(6).map { PaletteResult(id: $0.id.uuidString, title: $0.url.isEmpty ? L("Neue Seite") : $0.title, subtitle: L("Offener Tab · \($0.space)", "Open tab · \($0.space)"), icon: "rectangle.on.rectangle", tabID: $0.id, url: nil) }
        let addresses = Set(model.tabs.map(\.url))
        let recent = model.matchingHistory(term).filter { !addresses.contains($0.url) }.prefix(4)
            .map { PaletteResult(id: $0.id.uuidString, title: $0.title, subtitle: URL(string: $0.url)?.host ?? $0.url, icon: "clock", tabID: nil, url: $0.url) }
        let search = term.isEmpty ? [] : [PaletteResult(id: "search", title: L("„\(term)“ öffnen", "Open “\(term)”"), subtitle: L("URL öffnen oder mit DuckDuckGo suchen"), icon: "arrow.up.right", tabID: nil, url: term)]
        return open + recent + search
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 14) {
                Image(systemName: "magnifyingglass").font(.system(size: 20)).foregroundStyle(moss)
                TextField(L("Tabs, Verlauf oder das ganze Web …"), text: $query).textFieldStyle(.plain).font(.system(size: 18)).focused($focused).onSubmit { activate() }
                Button { model.showPalette = false } label: { Text("esc").font(.system(size: 10, design: .monospaced)).padding(6).background(moss.opacity(0.08), in: RoundedRectangle(cornerRadius: 5)) }.buttonStyle(YOBROButtonStyle()).keyboardShortcut(.cancelAction)
            }.padding(24)
            Divider().opacity(0.4)
            ScrollViewReader { reader in
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(query.isEmpty ? L("DEIN WEB, EINEN SPRUNG ENTFERNT") : L("TREFFER")).font(.system(size: 9, weight: .medium)).tracking(1.5).foregroundStyle(ink.opacity(0.4)).padding(.horizontal, 12).padding(.vertical, 13)
                        ForEach(Array(results.enumerated()), id: \.element.id) { index, result in
                            Button { selected = index; activate() } label: {
                                HStack(spacing: 14) {
                                    Image(systemName: result.icon).foregroundStyle(moss).frame(width: 21)
                                    VStack(alignment: .leading, spacing: 5) {
                                        Text(result.title).font(.system(size: 13, weight: .medium)).lineLimit(1)
                                        Text(result.subtitle).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5)).lineLimit(1)
                                    }
                                    Spacer()
                                    if index == selected { Image(systemName: "return").font(.system(size: 12)).foregroundStyle(moss) }
                                }.padding(12).background(index == selected ? moss.opacity(0.09) : .clear, in: RoundedRectangle(cornerRadius: 9))
                            }.buttonStyle(YOBROButtonStyle()).id(index)
                        }
                    }.padding(.horizontal, 12)
                }.onChange(of: selected) { _, value in reader.scrollTo(value) }
            }.frame(maxHeight: 420)
            HStack {
                Text(L("↑ ↓ auswählen     ↵ öffnen")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45))
                Spacer()
                Text("YoBro").font(.system(size: 9, weight: .medium, design: .monospaced)).tracking(2).foregroundStyle(moss)
            }.padding(18)
        }.frame(width: 610).background(paper).foregroundStyle(ink)
            .onAppear { focused = true }
            .onChange(of: query) { _, _ in selected = 0 }
            .onKeyPress(.downArrow) { selected = min(selected + 1, max(0, results.count - 1)); return .handled }
            .onKeyPress(.upArrow) { selected = max(0, selected - 1); return .handled }
    }
    private func activate() {
        guard results.indices.contains(selected) else { return }
        let result = results[selected]
        if let id = result.tabID { model.select(id) }
        else if let url = result.url { model.newTab(url: url) }
        model.showPalette = false
    }
}

struct PageFindBar: View {
    @ObservedObject var tab: BrowserTab
    @State private var query = ""
    @FocusState private var focused: Bool
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: "text.magnifyingglass").foregroundStyle(moss)
            TextField(L("Auf dieser Seite suchen"), text: $query).textFieldStyle(.plain).frame(minWidth: 80, idealWidth: 160, maxWidth: 210).focused($focused)
                .onSubmit { Task { await tab.find(query) } }
            if let found = tab.findFound { Text(found ? L("Treffer") : L("Kein Treffer")).font(.system(size: 10)).foregroundStyle(found ? moss : .orange) }
            if let error = tab.findError { Image(systemName: "exclamationmark.triangle").foregroundStyle(.orange).help(error) }
            Button { Task { await tab.find(tab.findQuery, backwards: true) } } label: { Image(systemName: "chevron.up") }.help(L("Vorheriger Treffer"))
            Button { Task { await tab.find(tab.findQuery) } } label: { Image(systemName: "chevron.down") }.help(L("Nächster Treffer"))
            Button { tab.showFind = false; Task { await tab.find("") } } label: { Image(systemName: "xmark") }.keyboardShortcut(.cancelAction)
        }.buttonStyle(YOBROButtonStyle()).font(.system(size: 12)).padding(7)
            .background(paper, in: RoundedRectangle(cornerRadius: 11)).shadow(color: .black.opacity(0.12), radius: 16, y: 6)
            .onAppear { query = tab.findQuery; focused = true }
            .onChange(of: tab.findQuery) { _, value in query = value }
            .task(id: query) {
                try? await Task.sleep(nanoseconds: 180_000_000)
                if !Task.isCancelled && query != tab.findQuery { await tab.find(query) }
            }
    }
}

struct LibraryButtons: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var downloads: DownloadStore
    var body: some View {
        HStack(spacing: 3) {
            Button { model.librarySection = .history } label: {
                Label(L("Verlauf", "History"), systemImage: "clock.arrow.circlepath")
                    .labelStyle(.iconOnly).frame(maxWidth: .infinity, minHeight: 28)
            }
            .help(L("Verlauf · ⌘Y"))
            .accessibilityLabel(L("Verlauf"))
            Button { model.librarySection = .bookmarks } label: {
                Label(L("Lesezeichen", "Bookmarks"), systemImage: "bookmark")
                    .labelStyle(.iconOnly).frame(maxWidth: .infinity, minHeight: 28)
            }
            .help(L("Lesezeichen · ⌥⌘B", "Bookmarks · ⌥⌘B"))
            .accessibilityLabel(L("Lesezeichen", "Bookmarks"))
            Button { model.librarySection = .downloads } label: {
                Label("Downloads", systemImage: "arrow.down.circle")
                    .labelStyle(.iconOnly).frame(maxWidth: .infinity, minHeight: 28)
            }
            .help("Downloads · ⇧⌘J")
            .accessibilityLabel("Downloads")
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 28))
        .font(.system(size: 12))
        .foregroundStyle(moss)
        .padding(3)
        .background(YOBROTheme.surface.opacity(0.20), in: RoundedRectangle(cornerRadius: 10))
    }
}
