import SwiftUI
import AppKit

enum CommandBarMode: Equatable {
    /// ⌘T: the destination opens in a new tab. No empty tab is created first.
    case newTab
    /// ⌘L: the destination replaces the active page.
    case current
}

struct CommandSuggestion: Identifiable, Equatable {
    enum Action: Equatable {
        case go(String)
        case tab(UUID)
        case reopen
    }
    let id: String
    let section: String
    let title: String
    let detail: String
    let symbol: String
    let action: Action
    var tabID: UUID? { if case .tab(let id) = action { return id }; return nil }
}

private extension Notification.Name {
    /// Sent with the model as object; the inline field of a blank tab takes focus.
    static let focusInlineCommandField = Notification.Name("YOBRO.focusInlineCommandField")
}

extension BrowserModel {
    /// Opens the address field. ⌘L on the start page has no page to replace,
    /// so it behaves like ⌘T. A blank tab already shows the field inline, so
    /// that one takes focus instead of a second field opening over it.
    func openCommandBar(_ mode: CommandBarMode) {
        closePeek()
        if let active, active.url.isEmpty {
            NotificationCenter.default.post(name: .focusInlineCommandField, object: self)
            return
        }
        commandBar = mode == .current && (active?.url.isEmpty ?? true) ? .newTab : mode
    }

    /// What the address field offers: a visited site whose host completes the
    /// typed text, the typed text itself, then open tabs, history and
    /// bookmarks. With nothing typed it lists recent tabs of this
    /// Space and the last closed tab, so ⌘T doubles as a tab switcher.
    func commandSuggestions(_ query: String) -> [CommandSuggestion] {
        let term = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let userTabs = tabs.filter { !agentTabIDs.contains($0.id) && !$0.url.isEmpty && $0.id != activeID }
        var result: [CommandSuggestion] = []
        if term.isEmpty {
            let recent = userTabs.filter { $0.space == space }.sorted { $0.lastActiveAt > $1.lastActiveAt }.prefix(6)
            result += recent.map(tabSuggestion)
            if let closed = closedTabs.last, !closed.url.isEmpty {
                result.append(CommandSuggestion(id: "reopen", section: L("Zuletzt geschlossen", "Recently closed"),
                                                title: closed.customTitle ?? closed.title, detail: URL(string: closed.url)?.host ?? closed.url,
                                                symbol: "arrow.uturn.backward", action: .reopen))
            }
            return result
        }
        let ranked = addressSuggestions(term, limit: 8)
        let topHit = term.count >= 2 && !term.contains(" ") && !term.contains("://") ? ranked.first { entry in
            // Like a browser's autocompletion: "git" offers github.com
            // above the search, but typing the whole host does not.
            guard let host = URL(string: entry.url)?.host?.lowercased() else { return false }
            let bare = host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
            return bare.hasPrefix(term.lowercased()) && bare != term.lowercased()
        } : nil
        let topHitTab = topHit.flatMap { entry in userTabs.first { $0.url == entry.url } }
        if let topHit {
            result.append(CommandSuggestion(id: "top", section: "", title: topHit.title,
                                            detail: URL(string: topHit.url)?.host ?? topHit.url, symbol: "arrow.up.right",
                                            action: topHitTab.map { .tab($0.id) } ?? .go(topHit.url)))
        }
        let looksLikeAddress = !term.contains(" ") && (term.contains(".") || term.contains("://") || term.hasPrefix("localhost"))
        result.append(CommandSuggestion(
            id: "go", section: "",
            title: term,
            detail: looksLikeAddress ? L("Öffnen", "Open") : L("Mit \(SearchEngine.current.name) suchen", "Search with \(SearchEngine.current.name)"),
            symbol: looksLikeAddress ? "globe" : "magnifyingglass", action: .go(term)))
        let matchingTabs = userTabs.filter { $0.id != topHitTab?.id && ($0.sidebarTitle.localizedCaseInsensitiveContains(term) || $0.url.localizedCaseInsensitiveContains(term)) }.prefix(4)
        result += matchingTabs.map(tabSuggestion)
        let open = Set(tabs.map(\.url))
        result += ranked.prefix(5).filter { !open.contains($0.url) && $0.url != topHit?.url }.map { entry in
            CommandSuggestion(id: "h-\(entry.id)", section: L("Verlauf", "History"), title: entry.title,
                              detail: URL(string: entry.url)?.host ?? entry.url, symbol: "clock", action: .go(entry.url))
        }
        let shown = Set(result.compactMap { suggestion -> String? in if case .go(let url) = suggestion.action { return url }; return nil })
        result += bookmarks.filter { !shown.contains($0.url) && !open.contains($0.url) && ($0.title.localizedCaseInsensitiveContains(term) || $0.url.localizedCaseInsensitiveContains(term)) }
            .prefix(3).map { entry in
                CommandSuggestion(id: "b-\(entry.id)", section: L("Lesezeichen", "Bookmarks"), title: entry.title,
                                  detail: URL(string: entry.url)?.host ?? entry.url, symbol: "bookmark", action: .go(entry.url))
            }
        return result
    }

    private func tabSuggestion(_ tab: BrowserTab) -> CommandSuggestion {
        CommandSuggestion(id: "t-\(tab.id)", section: L("Zu Tab wechseln", "Switch to tab"), title: tab.sidebarTitle,
                          detail: tab.space == space ? (URL(string: tab.url)?.host ?? tab.url) : tab.space,
                          symbol: "square.on.square", action: .tab(tab.id))
    }

    /// Runs a suggestion and closes the field. A blank page in the active tab
    /// is reused instead of stacking a second empty tab next to it.
    func perform(_ suggestion: CommandSuggestion, mode: CommandBarMode) {
        commandBar = nil
        switch suggestion.action {
        case .tab(let id):
            select(id)
        case .reopen:
            _ = reopenTab()
        case .go(let text):
            // Check first, so an address YoBro cannot open never leaves an
            // empty new tab behind.
            do { _ = try BrowserTab.resolveAddress(text) } catch { notice = error.localizedDescription; return }
            if let active, mode == .current || active.url.isEmpty {
                do { try active.navigate(text) } catch { notice = error.localizedDescription }
            } else {
                newTab(url: text)
            }
        }
    }
}

/// YAB's centred command field. Shown over the page for ⌘T/⌘L, and inline on
/// a blank tab.
struct CommandBar: View {
    @ObservedObject var model: BrowserModel
    let mode: CommandBarMode
    var inline = false
    @State private var query = ""
    @State private var selected = 0
    /// The suggestions are computed once per query, not on every redraw.
    @State private var list: [CommandSuggestion] = []
    /// Whether each row starts a new section and shows its heading.
    @State private var headings: [Bool] = []
    @State private var listQuery: String?
    @FocusState private var focused: Bool
    @Environment(\.colorScheme) private var colorScheme

    private var isPrivate: Bool { model.active?.isPrivate == true && (mode == .current || model.active?.url.isEmpty == true) }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(systemName: isPrivate ? "eyeglasses" : "magnifyingglass")
                    .font(.system(size: 18)).foregroundStyle(ink.opacity(0.45)).frame(width: 22)
                TextField(isPrivate ? L("Privat suchen oder Adresse eingeben", "Search privately or enter address") : L("Suchen oder Adresse eingeben", "Search or enter address"), text: $query)
                    .textFieldStyle(.plain)
                    .font(.system(size: 20))
                    .focused($focused)
                    .onSubmit(run)
                    .accessibilityIdentifier("command-bar-field")
            }
            .padding(.horizontal, 16).frame(height: 58)
            if !list.isEmpty {
                ScrollViewReader { reader in
                    ScrollView {
                        VStack(alignment: .leading, spacing: 0) {
                            ForEach(Array(list.enumerated()), id: \.element.id) { index, suggestion in
                                if index < headings.count && headings[index] {
                                    Text(suggestion.section)
                                        .font(.system(size: 12)).foregroundStyle(ink.opacity(0.45))
                                        .padding(.horizontal, 8).padding(.top, 12).padding(.bottom, 6)
                                        .accessibilityAddTraits(.isHeader)
                                }
                                Button { selected = index; run() } label: { row(suggestion, highlighted: index == selected) }
                                    .buttonStyle(.plain)
                                    .accessibilityElement(children: .ignore)
                                    .accessibilityLabel(spokenLabel(suggestion))
                                    .accessibilityAddTraits(index == selected ? .isSelected : [])
                                    .id(index)
                            }
                        }
                        .padding(.horizontal, 8).padding(.bottom, 8)
                    }
                    .scrollIndicators(.hidden)
                    .frame(height: min(listHeight(list), inline ? 300 : 380))
                    .onChange(of: selected) { _, value in reader.scrollTo(value) }
                }
            }
        }
        .frame(width: inline ? 620 : 680)
        .background(boxFill, in: RoundedRectangle(cornerRadius: 26, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 26, style: .continuous).strokeBorder(Chrome.hairline, lineWidth: 0.5))
        .shadow(color: .black.opacity(inline ? 0.08 : 0.28), radius: inline ? 14 : 35, y: inline ? 6 : 18)
        .foregroundStyle(ink)
        .appearanceTask {
            if mode == .current, let url = model.active?.url, !url.isEmpty { query = url; refresh() }
            selected = 0
            await Task.yield()
            focused = true
            await Task.yield()
            if !query.isEmpty { NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil) }
        }
        .appearanceTask(id: query) {
            // Typing quickly ranks the history once for the last keystroke.
            if !query.isEmpty { try? await Task.sleep(nanoseconds: 60_000_000) }
            guard !Task.isCancelled else { return }
            refresh()
        }
        // The empty field lists open tabs. Not `onReceive(model.$tabs)`: it
        // resubscribes on every redraw and replays the value, so each refresh
        // caused the next one and a blank tab kept the main thread busy.
        .onChange(of: model.tabs.map(\.id)) { _, _ in if query.isEmpty { refresh() } }
        .onReceive(NotificationCenter.default.publisher(for: .focusInlineCommandField, object: model)) { _ in
            guard inline else { return }
            focused = true
            Task { @MainActor in
                await Task.yield()
                NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil)
            }
        }
        .onChange(of: query) { _, _ in selected = 0 }
        .onKeyPress(.downArrow) { move(1); return .handled }
        .onKeyPress(.upArrow) { move(-1); return .handled }
        .onExitCommand { if !inline { model.commandBar = nil } }
    }

    private func refresh() {
        let suggestions = model.commandSuggestions(query)
        list = suggestions
        headings = suggestions.indices.map { index in
            !suggestions[index].section.isEmpty && (index == 0 || suggestions[index - 1].section != suggestions[index].section)
        }
        listQuery = query
        selected = min(selected, max(0, suggestions.count - 1))
    }

    /// Moves the highlight and tells VoiceOver which row it reached, since
    /// focus stays in the text field.
    private func move(_ delta: Int) {
        if listQuery != query { refresh() }
        guard !list.isEmpty else { return }
        let next = min(max(0, selected + delta), list.count - 1)
        guard next != selected else { return }
        selected = next
        AccessibilityNotification.Announcement(spokenLabel(list[next])).post()
    }

    private func spokenLabel(_ suggestion: CommandSuggestion) -> String {
        suggestion.detail.isEmpty ? suggestion.title : "\(suggestion.title), \(suggestion.detail)"
    }

    /// A ScrollView takes all the height it is offered, so size it to its
    /// rows: 38 pt each, plus each section heading and the bottom inset.
    private func listHeight(_ list: [CommandSuggestion]) -> CGFloat {
        CGFloat(list.count) * 38 + CGFloat(headings.filter { $0 }.count) * 32 + 8
    }

    private var boxFill: Color {
        colorScheme == .dark ? Color(white: 0.13, opacity: 0.97) : Color(red: 250 / 255, green: 250 / 255, blue: 251 / 255, opacity: 0.97)
    }

    private func row(_ suggestion: CommandSuggestion, highlighted: Bool) -> some View {
        HStack(spacing: 12) {
            Group {
                if let id = suggestion.tabID, let icon = model.tabs.first(where: { $0.id == id })?.favicon {
                    Image(nsImage: icon).resizable().scaledToFit().frame(width: 18, height: 18).clipShape(RoundedRectangle(cornerRadius: 4))
                } else {
                    Image(systemName: suggestion.symbol).font(.system(size: 13)).foregroundStyle(ink.opacity(0.6))
                }
            }
            .frame(width: 22, height: 22)
            Text(suggestion.title).lineLimit(1)
            Text(suggestion.detail).foregroundStyle(ink.opacity(0.45)).lineLimit(1)
            Spacer(minLength: 8)
            if highlighted { Image(systemName: "return").font(.system(size: 11)).foregroundStyle(ink.opacity(0.4)) }
        }
        .font(.system(size: 13.5))
        .padding(.horizontal, 8).frame(height: 38)
        .background(highlighted ? ink.opacity(0.08) : .clear, in: RoundedRectangle(cornerRadius: Chrome.radius))
        .contentShape(Rectangle())
    }

    private func run() {
        // Return can arrive before the debounced refresh for the last keystroke.
        if listQuery != query { refresh() }
        if list.indices.contains(selected) {
            model.perform(list[selected], mode: mode)
        } else if !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            model.perform(CommandSuggestion(id: "go", section: "", title: query, detail: "", symbol: "globe", action: .go(query)), mode: mode)
        }
    }
}
