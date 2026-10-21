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

extension BrowserModel {
    /// Opens the address field. ⌘L on the start page has no page to replace,
    /// so it behaves like ⌘T.
    func openCommandBar(_ mode: CommandBarMode) {
        commandBar = mode == .current && (active?.url.isEmpty ?? true) ? .newTab : mode
    }

    /// What the address field offers: the typed text first, then open tabs,
    /// history and bookmarks. With nothing typed it lists recent tabs of this
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
        let looksLikeAddress = !term.contains(" ") && (term.contains(".") || term.contains("://") || term.hasPrefix("localhost"))
        result.append(CommandSuggestion(
            id: "go", section: "",
            title: term,
            detail: looksLikeAddress ? L("Öffnen", "Open") : L("Mit DuckDuckGo suchen", "Search with DuckDuckGo"),
            symbol: looksLikeAddress ? "globe" : "magnifyingglass", action: .go(term)))
        let matchingTabs = userTabs.filter { $0.sidebarTitle.localizedCaseInsensitiveContains(term) || $0.url.localizedCaseInsensitiveContains(term) }.prefix(4)
        result += matchingTabs.map(tabSuggestion)
        let open = Set(tabs.map(\.url))
        result += addressSuggestions(term, limit: 5).filter { !open.contains($0.url) }.map { entry in
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
    @FocusState private var focused: Bool
    @Environment(\.colorScheme) private var colorScheme

    private var suggestions: [CommandSuggestion] { model.commandSuggestions(query) }
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
            if !suggestions.isEmpty {
                ScrollViewReader { reader in
                    ScrollView {
                        VStack(alignment: .leading, spacing: 0) {
                            ForEach(Array(suggestions.enumerated()), id: \.element.id) { index, suggestion in
                                if !suggestion.section.isEmpty && (index == 0 || suggestions[index - 1].section != suggestion.section) {
                                    Text(suggestion.section)
                                        .font(.system(size: 12)).foregroundStyle(ink.opacity(0.45))
                                        .padding(.horizontal, 8).padding(.top, 12).padding(.bottom, 6)
                                }
                                row(suggestion, highlighted: index == selected).id(index)
                                    .onTapGesture { selected = index; run() }
                            }
                        }
                        .padding(.horizontal, 8).padding(.bottom, 8)
                    }
                    .scrollIndicators(.hidden)
                    .frame(height: min(listHeight, inline ? 300 : 380))
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
            if mode == .current, let url = model.active?.url, !url.isEmpty { query = url }
            selected = 0
            await Task.yield()
            focused = true
            await Task.yield()
            if !query.isEmpty { NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil) }
        }
        .onChange(of: query) { _, _ in selected = 0 }
        .onKeyPress(.downArrow) { selected = min(selected + 1, max(0, suggestions.count - 1)); return .handled }
        .onKeyPress(.upArrow) { selected = max(0, selected - 1); return .handled }
        .onExitCommand { if !inline { model.commandBar = nil } }
    }

    /// A ScrollView takes all the height it is offered, so size it to its
    /// rows: 38 pt each, plus each section heading and the bottom inset.
    private var listHeight: CGFloat {
        let list = suggestions
        let headings = list.indices.filter { index in
            !list[index].section.isEmpty && (index == 0 || list[index - 1].section != list[index].section)
        }.count
        return CGFloat(list.count) * 38 + CGFloat(headings) * 32 + 8
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
        let list = suggestions
        if list.indices.contains(selected) {
            model.perform(list[selected], mode: mode)
        } else if !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            model.perform(CommandSuggestion(id: "go", section: "", title: query, detail: "", symbol: "globe", action: .go(query)), mode: mode)
        }
    }
}
