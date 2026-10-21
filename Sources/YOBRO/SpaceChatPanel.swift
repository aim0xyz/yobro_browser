import SwiftUI
import AppKit

struct SpaceChatPanel: View {
    @ObservedObject var browser: BrowserModel
    @ObservedObject var chat: SpaceChatStore
    @State private var settings = false
    @State private var sentDetailsExpanded = false
    @State private var chatScrollTask: Task<Void, Never>?
    @State private var freeModelsFirst = false

    private var draft: Binding<String> {
        Binding(get: { chat.drafts[browser.space] ?? "" }, set: { chat.drafts[browser.space] = $0 })
    }
    private var canSend: Bool {
        chat.isConfigured && !chat.running && !chat.historyUnavailable
            && !draft.wrappedValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        let rows = chat.entries.filter { $0.space == browser.space }
        VStack(spacing: 0) {
            header
            VStack(spacing: 10) {
                if !chat.isConfigured { connectionPrompt }
                if chat.historyUnavailable { historyRecovery }
                conversation(rows)
                if let error = chat.error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .font(.system(size: 11)).foregroundStyle(.orange).lineLimit(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                composer
                footer
            }
            .padding(.horizontal, 12).padding(.bottom, 12)
        }
        .background(paper)
        .onDisappear { chatScrollTask?.cancel() }
    }

    private var header: some View {
        HStack(spacing: 4) {
            VStack(alignment: .leading, spacing: 1) {
                Text(L("Assistent", "Assistant")).font(.system(size: 13, weight: .semibold))
                Text(browser.space).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            if chat.running {
                headerButton("stop.fill", L("Aktuelle Aktion stoppen", "Stop current action")) { browser.stopAgentRun() }
                    .foregroundStyle(.red)
            }
            headerButton("square.and.pencil", L("Neuer Chat", "New chat")) { chat.newChat(space: browser.space) }
            headerButton("slider.horizontal.3", L("Modell einstellen", "Model settings")) { settings.toggle() }
                .popover(isPresented: $settings, arrowEdge: .leading) {
                    ScrollView { connectionSettings.padding(16) }
                        .frame(width: 360, height: 460)
                        .background(paper)
                        .appearanceTask {
                            if ChatProviderPreset.matching(chat.connection.endpoint) == .openRouter,
                               !chat.apiKey.isEmpty, chat.openRouterModels.isEmpty {
                                await chat.loadOpenRouterModels()
                            }
                        }
                }
            headerButton("xmark", L("Assistent schließen", "Close assistant")) { browser.showAgent = false }
        }
        .padding(.leading, 14).padding(.trailing, 8).frame(height: 52)
    }

    private func headerButton(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 13)).frame(width: 30, height: 30)
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 30))
        .foregroundStyle(ink.opacity(0.7))
        .yobroHelp(help).accessibilityLabel(help)
    }

    private var connectionPrompt: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(L("Modell verbinden", "Connect a model")).font(.system(size: 13, weight: .semibold))
            Text(L("Der Assistent nutzt dein eigenes Konto bei OpenRouter oder einen OpenAI-kompatiblen Server.", "The assistant uses your own OpenRouter account or an OpenAI-compatible server."))
                .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Button(L("Einrichten", "Set up")) { settings = true }
                .buttonStyle(YOBROPopupButtonStyle(prominent: true))
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
    }

    private var historyRecovery: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(L("Verlauf konnte nicht geladen werden.", "Chat history could not be loaded."), systemImage: "exclamationmark.triangle.fill")
                .font(.system(size: 12, weight: .semibold)).foregroundStyle(.orange)
            Text(L("Die beschädigte Datei bleibt erhalten und wird beiseitegelegt.", "The damaged file is kept and moved aside."))
                .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Button(L("Neuen Verlauf starten", "Start new history")) { chat.resetCorruptHistory() }
                .buttonStyle(YOBROPopupButtonStyle(prominent: true))
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
    }

    private var connectionSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(L("Modell", "Model")).font(.system(size: 15, weight: .semibold))
            Picker(L("Anbieter", "Provider"), selection: providerSelection) {
                ForEach(ChatProviderPreset.allCases) { provider in Text(provider.title).tag(provider) }
            }
            .pickerStyle(.segmented).labelsHidden()
            if ChatProviderPreset.matching(chat.connection.endpoint) == .openRouter {
                openRouterSettings
            } else {
                TextField(L("Chat-Completions-Endpunkt", "Chat Completions endpoint"), text: Binding(get: { chat.connection.endpoint }, set: { chat.setEndpoint($0) }))
                Text(L("Beispiel: https://anbieter.example/v1/chat/completions oder ein lokaler Server.", "Example: https://provider.example/v1/chat/completions or a local server."))
                    .font(.system(size: 10)).foregroundStyle(.secondary)
                TextField(L("Modellname", "Model name"), text: $chat.connection.model)
            }
            SecureField(chat.apiKey.isEmpty ? L("API-Schlüssel", "API key") : L("API-Schlüssel gespeichert", "API key saved"), text: $chat.apiKey)
            Text(L("Der Schlüssel liegt nur im macOS-Schlüsselbund dieses Mac.", "The key is stored only in this Mac's Keychain."))
                .font(.system(size: 10)).foregroundStyle(.secondary)
            modelDataNotice
            HStack {
                Spacer()
                Button(L("Speichern", "Save")) {
                    do { try chat.saveConnection(); settings = false }
                    catch { chat.error = error.localizedDescription }
                }
                .buttonStyle(YOBROPopupButtonStyle(prominent: true))
            }
        }
        .textFieldStyle(YOBROTextFieldStyle())
        .disabled(chat.running)
    }

    @ViewBuilder
    private var openRouterSettings: some View {
        if chat.apiKey.isEmpty {
            HStack {
                Button {
                    chat.startOpenRouterConnect()
                } label: {
                    Label(chat.loadingOpenRouterModels ? L("OpenRouter wird geöffnet …", "Opening OpenRouter…") : L("Mit OpenRouter verbinden", "Connect with OpenRouter"), systemImage: "person.crop.circle.badge.checkmark")
                }
                .buttonStyle(YOBROPopupButtonStyle(prominent: true))
                .disabled(chat.loadingOpenRouterModels)
                if chat.loadingOpenRouterModels {
                    Button(L("Abbrechen", "Cancel")) { chat.cancelOpenRouterConnect() }
                        .buttonStyle(YOBROPopupButtonStyle())
                }
            }
        } else {
            HStack {
                Button(L("Trennen", "Disconnect")) { chat.disconnectOpenRouter() }
                    .buttonStyle(YOBROPopupButtonStyle())
                Button(L("Modelle laden", "Load models")) { Task { await chat.loadOpenRouterModels() } }
                    .buttonStyle(YOBROPopupButtonStyle())
                    .disabled(chat.loadingOpenRouterModels)
            }
        }
        if !chat.openRouterModels.isEmpty {
            Picker(L("Modell", "Model"), selection: $chat.connection.model) {
                Text("Auto Router").tag("openrouter/auto")
                if chat.connection.model != "openrouter/auto" && !chat.openRouterModels.contains(where: { $0.id == chat.connection.model && $0.supportsTools }) {
                    Text(chat.connection.model).tag(chat.connection.model)
                }
                ForEach(OpenRouterOAuth.sortedModels(chat.openRouterModels.filter(\.supportsTools).filter { $0.id != "openrouter/auto" }, freeFirst: freeModelsFirst)) { model in
                    Text(model.isFree ? "FREE · \(model.name ?? model.id)" : (model.name ?? model.id)).tag(model.id)
                }
            }
            .pickerStyle(.menu)
            Toggle(L("Kostenlose Modelle zuerst", "Free models first"), isOn: $freeModelsFirst)
                .toggleStyle(.checkbox).font(.system(size: 11))
            if chat.connection.model == "openrouter/auto" {
                Text(L("Auto Router kann ein Modell ohne Browser-Aktionen wählen. Falls eine Anfrage scheitert, wähle ein Modell mit Tool-Unterstützung.",
                       "Auto Router can pick a model without browser actions. If a request fails, choose a model with tool support."))
                    .font(.system(size: 10)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        } else {
            TextField(L("Modell", "Model"), text: $chat.connection.model)
        }
    }

    private var providerSelection: Binding<ChatProviderPreset> {
        Binding(
            get: { ChatProviderPreset.matching(chat.connection.endpoint) },
            set: { chat.selectProvider($0) }
        )
    }

    private func conversation(_ rows: [SpaceChatEntry]) -> some View {
        ScrollViewReader { proxy in
            ScrollView {
                // The store keeps at most SpaceChatStore.rowLimit unsaved rows
                // per Space. Keeping them realized avoids an AppKit/SwiftUI lazy
                // scroll bug where removing the thinking row can leave the
                // viewport beyond the content after a second response.
                VStack(spacing: 10) {
                    if rows.isEmpty { emptyState }
                    ForEach(rows) { entry in ChatEntryBubble(entry: entry) { browser.peek($0) } }
                    if chat.running {
                        AgentThinkingBubble()
                            .id("agent-thinking")
                            .transition(.move(edge: .bottom).combined(with: .opacity))
                    }
                    Color.clear.frame(height: 1).id("end")
                }.padding(.vertical, 2)
            }
            .scrollIndicators(.hidden)
            .onChange(of: rows.last?.id) { _, _ in scheduleScrollToBottom(proxy, animated: true) }
            .onChange(of: chat.running) { _, _ in scheduleScrollToBottom(proxy, animated: true) }
            .onChange(of: browser.space) { _, _ in scheduleScrollToBottom(proxy, animated: false) }
            .onAppear { scheduleScrollToBottom(proxy, animated: false) }
        }.frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func scheduleScrollToBottom(_ proxy: ScrollViewProxy, animated: Bool) {
        chatScrollTask?.cancel()
        chatScrollTask = Task { @MainActor in
            // The later passes wait for text wrapping and the panel transition
            // (or the removal of the thinking row) to settle.
            await Task.yield()
            guard !Task.isCancelled else { return }
            if animated { withAnimation(.easeOut(duration: 0.22)) { proxy.scrollTo("end", anchor: .bottom) } }
            else { proxy.scrollTo("end", anchor: .bottom) }
            do { try await Task.sleep(for: .milliseconds(60)) } catch { return }
            proxy.scrollTo("end", anchor: .bottom)
            do { try await Task.sleep(for: .milliseconds(180)) } catch { return }
            proxy.scrollTo("end", anchor: .bottom)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 10) {
            Image(systemName: "sparkles").font(.system(size: 22, weight: .light)).foregroundStyle(ink.opacity(0.45))
            Text(L("Was soll ich für dich erledigen?", "What should I do for you?"))
                .font(.system(size: 13, weight: .semibold))
            Text(L("Ich lese die Tabs dieses Spaces und arbeite in einem eigenen Tab daneben. Private Tabs sehe ich nicht.", "I read this Space's tabs and work in my own tab beside yours. I never see private tabs."))
                .font(.system(size: 11)).foregroundStyle(.secondary).multilineTextAlignment(.center).fixedSize(horizontal: false, vertical: true)
            if chat.isConfigured {
                HStack(spacing: 6) {
                    suggestion(L("Diese Seite zusammenfassen", "Summarize this page"))
                    suggestion(L("Was steht in meinen Tabs?", "What's in my tabs?"))
                }
            }
        }.frame(maxWidth: 260).padding(.vertical, 40)
    }

    private func suggestion(_ text: String) -> some View {
        Button(text) { chat.drafts[browser.space] = text }
            .buttonStyle(.plain).font(.system(size: 11)).foregroundStyle(ink.opacity(0.75))
            .padding(.horizontal, 10).frame(height: 28)
            .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 8))
    }

    private var composer: some View {
        HStack(alignment: .bottom, spacing: 8) {
            ZStack(alignment: .topLeading) {
                if draft.wrappedValue.isEmpty {
                    Text(L("Frag oder gib eine Aufgabe …", "Ask or give a task …"))
                        .font(.system(size: 13)).foregroundStyle(.secondary)
                        .padding(.leading, 5).padding(.top, 4)
                        .allowsHitTesting(false)
                }
                ChatMessageInput(text: draft) {
                    guard canSend else { return }
                    chat.send(browser: browser)
                }
            }
            .yobroHelp(L("Enter: senden · Shift+Enter: neue Zeile", "Enter: send · Shift+Enter: new line"))
            if chat.running {
                ProgressView().controlSize(.small).frame(width: 28, height: 28)
            } else {
                Button { chat.send(browser: browser) } label: {
                    Image(systemName: "arrow.up").font(.system(size: 12, weight: .bold)).foregroundStyle(paper)
                        .frame(width: 28, height: 28).background(ink, in: Circle())
                }
                .buttonStyle(.plain)
                .disabled(!canSend)
                .opacity(canSend ? 1 : 0.35)
                .yobroHelp(L("Senden", "Send"))
            }
        }
        .padding(10)
        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 6) {
            Button { sentDetailsExpanded.toggle() } label: {
                HStack(spacing: 6) {
                    Image(systemName: isLocalModelEndpoint ? "desktopcomputer" : "cloud")
                    Text(isLocalModelEndpoint
                         ? L("Lokales Modell", "Local model")
                         : L("Geht an \(modelProviderName)", "Sent to \(modelProviderName)"))
                    Image(systemName: sentDetailsExpanded ? "chevron.up" : "chevron.down").font(.system(size: 8, weight: .semibold))
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .font(.system(size: 10)).foregroundStyle(.secondary)
            if sentDetailsExpanded {
                Text(dataDisclosureDetail)
                    .font(.system(size: 10)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.horizontal, 4)
    }

    private var isLocalModelEndpoint: Bool {
        guard let host = URL(string: chat.connection.endpoint)?.host?.lowercased() else { return false }
        return ["localhost", "127.0.0.1", "::1", "[::1]"].contains(host)
    }

    private var modelProviderName: String {
        let preset = ChatProviderPreset.matching(chat.connection.endpoint)
        if preset != .custom { return preset.title }
        return URL(string: chat.connection.endpoint)?.host ?? L("deinen Modellanbieter", "your model provider")
    }

    private var modelDataNotice: some View {
        Label {
            Text(isLocalModelEndpoint
                 ? L("Lokale AI: Chat und gelesene Seiten gehen nur an deinen lokalen Server.", "Local AI: chat and pages read go only to your local server.")
                 : L("Cloud-AI: Chat und alles, was der Assistent liest, gehen an \(modelProviderName).", "Cloud AI: chat and everything the assistant reads go to \(modelProviderName)."))
                .fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: isLocalModelEndpoint ? "desktopcomputer" : "cloud.fill")
        }
        .font(.system(size: 10))
        .foregroundStyle(.secondary)
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 9))
    }

    private var dataDisclosureDetail: String {
        L("YoBro ersetzt erkannte E-Mail-Adressen, Zugangsdaten, Telefonnummern und Postadressen vor jeder Anfrage durch Platzhalter und setzt Antworten hier wieder zusammen. Gesendet werden bis zu 30 Chat-Nachrichten, Titel und Adressen der nicht privaten Tabs dieses Spaces und Seiten, die der Assistent liest. Nicht erkannte private Angaben können trotzdem beim Anbieter landen.",
          "YoBro replaces detected email addresses, credentials, phone numbers and postal addresses with placeholders before every request and restores replies locally. Up to 30 chat messages, the titles and addresses of this Space's non-private tabs, and pages the assistant reads are sent. Undetected private data may still reach the provider.")
    }
}

private struct AgentThinkingBubble: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        TimelineView(.animation(minimumInterval: reduceMotion ? 1 : 1.0 / 30.0)) { context in
            let phase = reduceMotion ? 0 : context.date.timeIntervalSinceReferenceDate * 4.2
            HStack(spacing: 5) {
                ForEach(0..<3, id: \.self) { index in
                    let wave = (sin(phase - Double(index) * 0.9) + 1) / 2
                    Circle()
                        .fill(ink.opacity(0.25 + wave * 0.45))
                        .frame(width: 6, height: 6)
                        .offset(y: reduceMotion ? 0 : -wave * 3)
                }
            }
            .frame(height: 11, alignment: .bottom)
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(L("Assistent denkt und erstellt eine Antwort", "Assistant is thinking and composing a response"))
    }
}

/// Assistant replies use inline Markdown: emphasis, code and links. Block
/// syntax such as lists stays as typed, with its line breaks.
enum ChatMarkdown {
    static func attributed(_ text: String) -> AttributedString {
        (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(text)
    }
}

private struct ChatEntryBubble: View {
    let entry: SpaceChatEntry
    let openLink: (URL) -> Void
    private let rendered: AttributedString
    @State private var copied = false
    @State private var copyResetTask: Task<Void, Never>?
    private var isUser: Bool { entry.kind == "user" }

    init(entry: SpaceChatEntry, openLink: @escaping (URL) -> Void) {
        self.entry = entry
        self.openLink = openLink
        rendered = entry.kind == "assistant" ? ChatMarkdown.attributed(entry.text) : AttributedString()
    }

    var body: some View {
        Group {
            switch entry.kind {
            case "user":
                HStack {
                    Spacer(minLength: 40)
                    Text(entry.text).textSelection(.enabled)
                        .padding(.horizontal, 11).padding(.vertical, 8)
                        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
                }
            case "assistant":
                VStack(alignment: .leading, spacing: 6) {
                    Text(rendered).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                        // Links from the model open in a preview, and only web links.
                        .environment(\.openURL, OpenURLAction { url in
                            guard ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return .discarded }
                            openLink(url)
                            return .handled
                        })
                    Button(action: copy) {
                        Image(systemName: copied ? "checkmark" : "doc.on.doc").font(.system(size: 10))
                            .foregroundStyle(ink.opacity(0.5)).frame(width: 24, height: 22)
                    }
                    .buttonStyle(YOBROButtonStyle(minimumSize: 22))
                    .yobroHelp(copied ? L("Kopiert", "Copied") : L("Antwort kopieren", "Copy response"))
                    .accessibilityLabel(copied ? L("Kopiert", "Copied") : L("Antwort kopieren", "Copy response"))
                }
                .padding(.horizontal, 2)
            default:
                Label(entry.text, systemImage: entry.kind == "error" ? "exclamationmark.triangle" : "bolt")
                    .font(.system(size: 11))
                    .foregroundStyle(entry.kind == "error" ? Color.orange : Color.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 2)
            }
        }
        .font(.system(size: 13))
        .id(entry.id)
        .onDisappear { copyResetTask?.cancel() }
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        copied = NSPasteboard.general.setString(entry.text, forType: .string)
        copyResetTask?.cancel()
        copyResetTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            guard !Task.isCancelled else { return }
            copied = false
        }
    }
}
