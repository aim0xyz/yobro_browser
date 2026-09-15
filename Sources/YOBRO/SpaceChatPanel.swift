import SwiftUI
import AppKit
import LocalAuthentication

@MainActor
final class APIKeyRevealAuthenticator {
    func authenticate() async throws -> Bool {
        let context = LAContext()
        context.localizedCancelTitle = L("Abbrechen", "Cancel")
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
            throw error ?? YOBROError.message(L("Auf diesem Mac ist keine Geräteauthentifizierung eingerichtet.", "Device authentication is not configured on this Mac."))
        }
        return try await context.evaluatePolicy(
            .deviceOwnerAuthentication,
            localizedReason: L("API-Schlüssel in YoBro anzeigen und Kopieren erlauben", "Reveal the API key in YoBro and allow copying")
        )
    }
}

struct SpaceChatPanel: View {
    @ObservedObject var browser: BrowserModel
    @ObservedObject var chat: SpaceChatStore
    @State private var settings = false
    @State private var sentDetailsExpanded = false
    @State private var externalAgentAccessExpanded = false
    @State private var socketCopySucceeded: Bool?
    @State private var socketCopyReset: Task<Void, Never>?
    @State private var showAPIKey = false
    @State private var unlockingAPIKey = false
    @State private var apiKeyCopied = false
    @State private var apiKeyRelock: Task<Void, Never>?
    @State private var apiKeyClipboardClear: Task<Void, Never>?
    @State private var chatScrollTask: Task<Void, Never>?
    @State private var freeModelsFirst = false
    private let apiKeyAuthenticator = APIKeyRevealAuthenticator()

    private var rows: [SpaceChatEntry] { chat.entries.filter { $0.space == browser.space } }
    private var configured: Bool {
        !chat.connection.endpoint.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !chat.connection.model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }
    private var draft: Binding<String> {
        Binding(get: { chat.drafts[browser.space] ?? "" }, set: { chat.drafts[browser.space] = $0 })
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().opacity(0.55)
            VStack(spacing: 12) {
                if settings { connectionSettings }
                else if !configured { connectionPrompt }

                conversation
                if let url = chat.pendingURL { approval(url) }
                if let error = chat.error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .font(.caption).foregroundStyle(.orange).lineLimit(3)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                composer
                footer
            }.padding(14)
        }
        .background(YOBROTheme.chromeBottom.opacity(0.9))
        .overlay(alignment: .leading) {
            Rectangle()
                .fill(ink.opacity(0.16))
                .frame(width: 1)
                .frame(maxHeight: .infinity)
                .allowsHitTesting(false)
        }
        .onDisappear {
            socketCopyReset?.cancel()
            socketCopySucceeded = nil
            chatScrollTask?.cancel()
            lockAPIKey()
        }
        .onChange(of: chat.connection.endpoint) { _, _ in lockAPIKey() }
    }

    private var header: some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                Text("YoBro AGENT").font(.system(size: 9, weight: .bold, design: .rounded)).tracking(1.3).foregroundStyle(moss)
                Text(browser.space).font(.system(size: 16, weight: .semibold, design: .rounded)).lineLimit(1)
            }
            Spacer()
            statusPill
            headerButton("square.and.pencil", L("Neuer Chat", "New chat")) { chat.newChat(space: browser.space) }
            headerButton("slider.horizontal.3", L("Modell verbinden", "Connect model")) { settings.toggle() }
            headerButton("xmark", L("Assistent schließen", "Close assistant")) { browser.showAgent = false }
        }
        .padding(.horizontal, 12).padding(.top, 32).padding(.bottom, 12)
        .background(YOBROTheme.surface.opacity(0.16))
    }

    private var statusPill: some View {
        HStack(spacing: 5) {
            Circle().fill(chat.running ? Color.orange : configured ? moss : Color.secondary).frame(width: 6, height: 6)
            Text(chat.running ? L("Läuft", "Working") : configured ? L("Bereit", "Ready") : L("Setup", "Setup"))
                .font(.system(size: 9, weight: .semibold))
        }
        .padding(.horizontal, 8).frame(height: 25)
        .background(YOBROTheme.surface.opacity(0.45), in: Capsule())
    }

    private func headerButton(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 12, weight: .semibold)).frame(width: 30, height: 30)
                .background(YOBROTheme.surface.opacity(0.42), in: Circle())
        }.buttonStyle(.plain).help(help).accessibilityLabel(help)
    }

    private var connectionPrompt: some View {
        HStack(spacing: 11) {
            Image(systemName: "point.3.connected.trianglepath.dotted").font(.system(size: 20)).foregroundStyle(moss)
            VStack(alignment: .leading, spacing: 3) {
                Text(L("Modell verbinden", "Connect a model")).font(.system(size: 12, weight: .semibold))
                Text(L("Einmal einrichten, danach bleibt hier mehr Platz für deine Unterhaltung.", "Set it up once, then keep more room for your conversation."))
                    .font(.system(size: 10)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 4)
            Button(L("Einrichten", "Set up")) { settings = true }.buttonStyle(.bordered).controlSize(.small)
        }.padding(12).background(moss.opacity(0.09), in: RoundedRectangle(cornerRadius: 12))
    }

    private var connectionSettings: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label(L("Modellanschluss", "Model connection"), systemImage: "link").font(.system(size: 12, weight: .semibold))
                Spacer()
                Button { settings = false } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }.buttonStyle(.plain)
            }
            Picker(L("Anbieter", "Provider"), selection: providerSelection) {
                ForEach(ChatProviderPreset.allCases) { provider in Text(provider.title).tag(provider) }
            }
            .pickerStyle(.menu)
            if ChatProviderPreset.matching(chat.connection.endpoint) == .custom {
                TextField(L("Vollständiger Chat-Completions-Endpunkt", "Full Chat Completions endpoint"), text: $chat.connection.endpoint)
                Text(L("Beispiel: https://anbieter.example/v1/chat/completions", "Example: https://provider.example/v1/chat/completions"))
                    .font(.system(size: 9)).foregroundStyle(.secondary)
            } else {
                Text(chat.connection.endpoint)
                    .font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
            if ChatProviderPreset.matching(chat.connection.endpoint) == .openRouter && !chat.openRouterModels.isEmpty {
                Picker(L("Modell", "Model"), selection: $chat.connection.model) {
                    if !chat.openRouterModels.contains(where: { $0.id == chat.connection.model }) {
                        Text(chat.connection.model).tag(chat.connection.model)
                    }
                    ForEach(OpenRouterOAuth.sortedModels(chat.openRouterModels, freeFirst: freeModelsFirst)) { model in
                        Text(model.isFree ? "FREE · \(model.name ?? model.id)" : (model.name ?? model.id)).tag(model.id)
                    }
                }.pickerStyle(.menu)
                Toggle(L("Kostenlose Modelle zuerst", "Free models first"), isOn: $freeModelsFirst)
                    .toggleStyle(.checkbox).controlSize(.small).font(.system(size: 10))
            } else if ChatProviderPreset.matching(chat.connection.endpoint) == .anthropic {
                Picker(L("Claude-Modell", "Claude model"), selection: $chat.connection.model) {
                    if !ChatProviderPreset.anthropicModels.contains(chat.connection.model) {
                        Text(chat.connection.model).tag(chat.connection.model)
                    }
                    ForEach(ChatProviderPreset.anthropicModels, id: \.self) { Text($0).tag($0) }
                }.pickerStyle(.menu)
            } else {
                TextField(L("Modellname", "Model name"), text: $chat.connection.model)
            }
            if ChatProviderPreset.matching(chat.connection.endpoint) == .openRouter {
                HStack {
                    if chat.apiKey.isEmpty {
                        Button {
                            Task { await chat.connectOpenRouter() }
                        } label: {
                            Label(chat.loadingOpenRouterModels ? L("OpenRouter wird geöffnet …", "Opening OpenRouter…") : L("Mit OpenRouter verbinden", "Connect with OpenRouter"), systemImage: "person.crop.circle.badge.checkmark")
                        }
                        .buttonStyle(.borderedProminent).tint(moss).controlSize(.small)
                        .disabled(chat.loadingOpenRouterModels)
                    } else {
                        Button(role: .destructive) { chat.disconnectOpenRouter(); lockAPIKey() } label: {
                            Label(L("OpenRouter trennen", "Disconnect OpenRouter"), systemImage: "link.badge.minus")
                        }
                        .buttonStyle(.bordered).controlSize(.small)
                        Button(L("Modelle laden", "Load models")) { Task { await chat.loadOpenRouterModels() } }
                            .buttonStyle(.bordered).controlSize(.small).disabled(chat.loadingOpenRouterModels)
                    }
                }
                Text(L("OpenRouter erstellt nach deiner Freigabe einen eigenen Schlüssel für YoBro. Die manuelle Eingabe darunter bleibt verfügbar.", "After your approval, OpenRouter creates a dedicated key for YoBro. Manual entry below remains available."))
                    .font(.system(size: 9)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if ChatProviderPreset.matching(chat.connection.endpoint) == .anthropic {
                Link(L("Anthropic API-Key erstellen", "Create Anthropic API key"), destination: URL(string: "https://console.anthropic.com/settings/keys")!)
                    .font(.system(size: 10)).foregroundStyle(moss)
            }
            HStack(spacing: 6) {
                if showAPIKey || chat.apiKey.isEmpty {
                    Group {
                        if showAPIKey {
                            TextField(L("API-Schlüssel (sicher gespeichert)", "API key (stored securely)"), text: $chat.apiKey)
                        } else {
                            SecureField(L("API-Schlüssel (sicher gespeichert)", "API key (stored securely)"), text: $chat.apiKey)
                        }
                    }
                    .textFieldStyle(.plain)
                } else {
                    Text(String(repeating: "•", count: min(max(chat.apiKey.count, 12), 24)))
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .textSelection(.disabled)
                        .accessibilityLabel(L("API-Schlüssel verborgen", "API key hidden"))
                    Spacer(minLength: 0)
                }
                if showAPIKey, !chat.apiKey.isEmpty {
                    Button(action: copyAPIKey) {
                        Image(systemName: apiKeyCopied ? "checkmark" : "doc.on.doc")
                            .foregroundStyle(apiKeyCopied ? moss : Color.secondary)
                            .frame(width: 24, height: 24)
                    }
                    .buttonStyle(.plain)
                    .help(L("API-Schlüssel kopieren", "Copy API key"))
                    .accessibilityLabel(L("API-Schlüssel kopieren", "Copy API key"))
                }
                Button {
                    if showAPIKey { lockAPIKey() }
                    else { Task { await unlockAPIKey() } }
                } label: {
                    Image(systemName: unlockingAPIKey ? "ellipsis" : showAPIKey ? "eye.slash" : "eye")
                        .foregroundStyle(.secondary)
                        .frame(width: 24, height: 24)
                }
                .buttonStyle(.plain)
                .disabled(unlockingAPIKey || chat.apiKey.isEmpty)
                .help(showAPIKey ? L("API-Schlüssel verbergen", "Hide API key") : L("Mit macOS entsperren", "Unlock with macOS"))
                .accessibilityLabel(showAPIKey ? L("API-Schlüssel verbergen", "Hide API key") : L("API-Schlüssel sicher entsperren", "Securely unlock API key"))
            }
            .padding(.horizontal, 10)
            .frame(minHeight: 34)
            .background(YOBROTheme.surface.opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
            Label(L("Der Schlüssel wird verschlüsselt und nur auf diesem Mac im Schlüsselbund gespeichert – niemals im Verlauf oder in einer Konfigurationsdatei.", "The key is encrypted and stored only on this Mac in Keychain—never in history or a configuration file."), systemImage: "key.fill")
                .font(.system(size: 9)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            modelDataNotice
            HStack {
                Text(L("HTTPS oder lokaler OpenAI-kompatibler Server.", "HTTPS or a local OpenAI-compatible server."))
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Spacer()
                Button(L("Speichern", "Save")) {
                    do { try chat.saveConnection(); lockAPIKey(); settings = false } catch { chat.error = error.localizedDescription }
                }.buttonStyle(.borderedProminent).tint(moss).controlSize(.small)
            }
        }
        .textFieldStyle(YOBROTextFieldStyle()).padding(12)
        .background(YOBROTheme.surface.opacity(0.62), in: RoundedRectangle(cornerRadius: 12))
        .disabled(chat.running)
    }

    private func unlockAPIKey() async {
        guard !chat.apiKey.isEmpty, !unlockingAPIKey else { return }
        unlockingAPIKey = true
        defer { unlockingAPIKey = false }
        do {
            guard try await apiKeyAuthenticator.authenticate() else { return }
            showAPIKey = true
            apiKeyRelock?.cancel()
            apiKeyRelock = Task { @MainActor in
                try? await Task.sleep(nanoseconds: 60_000_000_000)
                guard !Task.isCancelled else { return }
                lockAPIKey()
            }
        } catch {
            let nsError = error as NSError
            guard nsError.domain == LAError.errorDomain,
                  [LAError.userCancel.rawValue, LAError.appCancel.rawValue, LAError.systemCancel.rawValue].contains(nsError.code) else {
                chat.error = error.localizedDescription
                return
            }
        }
    }

    private func lockAPIKey() {
        showAPIKey = false
        apiKeyCopied = false
        apiKeyRelock?.cancel()
        apiKeyRelock = nil
    }

    private func copyAPIKey() {
        guard showAPIKey, !chat.apiKey.isEmpty else { return }
        NSPasteboard.general.clearContents()
        guard NSPasteboard.general.setString(chat.apiKey, forType: .string) else { return }
        let change = NSPasteboard.general.changeCount
        apiKeyCopied = true
        apiKeyClipboardClear?.cancel()
        apiKeyClipboardClear = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 60_000_000_000)
            guard !Task.isCancelled else { return }
            if NSPasteboard.general.changeCount == change { NSPasteboard.general.clearContents() }
            apiKeyCopied = false
        }
    }

    private var providerSelection: Binding<ChatProviderPreset> {
        Binding(
            get: { ChatProviderPreset.matching(chat.connection.endpoint) },
            set: { chat.selectProvider($0) }
        )
    }

    private var conversation: some View {
        ScrollViewReader { proxy in
            ScrollView {
                // Chat rows are persisted and bounded by the conversation use
                // case. Keeping them realized avoids an AppKit/SwiftUI lazy
                // scroll bug where removing the thinking row can leave the
                // viewport beyond the content after a second response.
                VStack(spacing: 12) {
                    if rows.isEmpty { emptyState }
                    ForEach(rows) { entry in ChatEntryBubble(entry: entry, chat: chat) }
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
        VStack(spacing: 12) {
            Image(systemName: "sparkles.rectangle.stack")
                .font(.system(size: 28, weight: .light)).foregroundStyle(moss.opacity(0.8))
            Text(L("Was möchtest du erledigen?", "What would you like to do?"))
                .font(.system(size: 14, weight: .semibold))
            Text(L("Der Agent arbeitet mit den Tabs, Notizen und dem Mailbereich dieses Spaces.", "The agent works with this Space's tabs, notes, and mail area."))
                .font(.system(size: 11)).foregroundStyle(.secondary).multilineTextAlignment(.center).fixedSize(horizontal: false, vertical: true)
            if configured {
                HStack(spacing: 7) {
                    suggestion(L("Tabs zusammenfassen", "Summarize tabs"))
                    suggestion(L("Diese Seite erklären", "Explain this page"))
                }
            }
        }.frame(maxWidth: 290).padding(.vertical, 34)
    }

    private func suggestion(_ text: String) -> some View {
        Button(text) { chat.drafts[browser.space] = text }
            .buttonStyle(.plain).font(.system(size: 10, weight: .medium)).foregroundStyle(moss)
            .padding(.horizontal, 10).frame(height: 30).background(moss.opacity(0.1), in: Capsule())
    }

    private func approval(_ url: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(L("Diese Seite öffnen?", "Open this page?"), systemImage: "hand.raised.fill").font(.system(size: 12, weight: .semibold))
            Text(url).font(.caption).lineLimit(2).textSelection(.enabled)
            Text(L("Die Seite öffnet sich sichtbar in einem Tab.", "The page opens visibly in a tab."))
                .font(.system(size: 10)).foregroundStyle(.secondary)
            HStack {
                Button(L("Ablehnen", "Decline")) { chat.resolve(false) }
                Spacer()
                Button(L("Freigeben", "Approve")) { chat.resolve(true) }.buttonStyle(.borderedProminent).tint(moss)
            }.controlSize(.small)
        }.padding(12).background(moss.opacity(0.1), in: RoundedRectangle(cornerRadius: 12))
    }

    private var composer: some View {
        VStack(alignment: .leading, spacing: 9) {
            if let context = chat.contexts[browser.space] {
                HStack(spacing: 6) {
                    Image(systemName: "envelope.fill").foregroundStyle(moss)
                    Text(L("Mail-Kontext angehängt", "Email context attached")).font(.system(size: 10, weight: .medium))
                    Spacer()
                    Button { chat.contexts[browser.space] = nil } label: { Image(systemName: "xmark") }
                        .buttonStyle(.plain).help(L("Kontext entfernen", "Remove context"))
                }
                .padding(.horizontal, 9).frame(height: 27).background(moss.opacity(0.09), in: Capsule())
                .help(String(context.prefix(300)))
            }
            messageField
            HStack {
                Label(L("Space-Kontext", "Space context"), systemImage: "square.stack.3d.up")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Spacer()
                if chat.running {
                    ProgressView().controlSize(.small)
                    Button(L("Stopp", "Stop")) { chat.cancel() }.buttonStyle(.plain).font(.caption)
                } else {
                    Button { chat.send(browser: browser) } label: {
                        Image(systemName: "arrow.up").font(.system(size: 12, weight: .bold)).foregroundStyle(paper)
                            .frame(width: 30, height: 30).background(moss, in: Circle())
                    }
                    .buttonStyle(.plain)
                    .disabled(!browser.agentEnabled || !configured || draft.wrappedValue.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .help(L("Senden", "Send"))
                }
            }
        }
        .padding(12).background(YOBROTheme.surface.opacity(0.68), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(ink.opacity(0.08)))
    }

    private var messageField: some View {
        TextField(L("Nachricht an diesen Space …", "Message this Space …"), text: draft, axis: .vertical)
            .lineLimit(2...5)
            .textFieldStyle(.plain)
            .font(.system(size: 12))
            .onKeyPress(keys: [.return], phases: .down) { press in
                if press.modifiers.contains(.shift) { return .ignored }
                chat.send(browser: browser)
                return .handled
            }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 7) {
            if !settings { modelDataNotice }
            Button { sentDetailsExpanded.toggle() } label: {
                footerHeader(
                    L("Was wird gesendet?", "What is sent?"),
                    icon: "lock.shield",
                    expanded: sentDetailsExpanded
                )
            }.buttonStyle(.plain)
            if sentDetailsExpanded {
                Text(dataDisclosureDetail)
                    .font(.system(size: 9)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    .padding(.leading, 29).padding(.trailing, 4)
            }
            Button { externalAgentAccessExpanded.toggle() } label: {
                footerHeader(
                    L("Browserzugriff für Agenten", "Browser access for agents"),
                    icon: "externaldrive.connected.to.line.below",
                    expanded: externalAgentAccessExpanded
                )
            }.buttonStyle(.plain)
            if externalAgentAccessExpanded {
                VStack(alignment: .leading, spacing: 7) {
                Text(L("Erlaubt Agent-Harnesses, Codex, Claude und unterstützten IDEs, YoBro über die lokale Schnittstelle zu steuern.",
                       "Lets agent harnesses, Codex, Claude, and supported IDEs control YoBro through its local interface."))
                    .font(.system(size: 9)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Toggle(L("Agenten Zugriff erlauben", "Allow agent access"), isOn: $browser.agentEnabled)
                    .toggleStyle(.checkbox)
                    .frame(maxWidth: .infinity, alignment: .leading)
                HStack {
                    Text(browser.bridgeStatus).foregroundStyle(.secondary)
                    Spacer()
                    Button {
                        socketCopyReset?.cancel()
                        NSPasteboard.general.clearContents()
                        socketCopySucceeded = NSPasteboard.general.setString(browser.controlSocketURL.path, forType: .string)
                        socketCopyReset = Task { @MainActor in
                            do { try await Task.sleep(for: .seconds(2)) }
                            catch { return }
                            socketCopySucceeded = nil
                        }
                    } label: {
                        if let succeeded = socketCopySucceeded {
                            Label(succeeded ? L("Kopiert", "Copied") : L("Kopieren fehlgeschlagen", "Copy failed"),
                                  systemImage: succeeded ? "checkmark" : "exclamationmark.triangle")
                        } else {
                            Text(L("Verbindungsadresse kopieren", "Copy connection address"))
                        }
                    }.buttonStyle(.borderless)
                }
                }.padding(.leading, 29).padding(.trailing, 4)
            }
        }.font(.system(size: 10)).padding(.horizontal, 3)
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
                 ? L("Lokale AI: YoBro sendet Agent-Daten nur an deinen lokalen Server.", "Local AI: YoBro sends agent data only to your local server.")
                 : L("Cloud-AI: Chat und alles, was der Agent liest, gehen an \(modelProviderName).", "Cloud AI: Chat and everything the agent reads is sent to \(modelProviderName)."))
                .fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: isLocalModelEndpoint ? "desktopcomputer" : "cloud.fill")
        }
        .font(.system(size: 9, weight: .medium))
        .foregroundStyle(isLocalModelEndpoint ? moss : Color.orange)
        .padding(.horizontal, 9).padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background((isLocalModelEndpoint ? moss : Color.orange).opacity(0.09), in: RoundedRectangle(cornerRadius: 9))
    }

    private var dataDisclosureDetail: String {
        if isLocalModelEndpoint {
            return L("YoBro sendet bis zu 30 letzte Chat-Nachrichten, Tab-/Notiz-Metadaten, ausgewählten Mail-Kontext und alle vom Agenten abgerufenen Mail-, Seiten- und Notizinhalte nur an den lokalen Endpunkt. Ob die lokale Serversoftware Daten weiterleitet, liegt außerhalb von YoBro.",
                     "YoBro sends up to 30 recent chat messages, tab/note metadata, selected email context, and all email, page, and note content retrieved by the agent only to the local endpoint. Whether the local server software forwards data is outside YoBro's control.")
        }
        return L("Bei jeder Anfrage gehen bis zu 30 letzte Chat-Nachrichten, Tab-/Notiz-Metadaten und ausgewählter Mail-Kontext an \(modelProviderName). Wenn der Agent Mails, Webseiten oder Notizen liest, wird auch der dabei abgerufene Inhalt für die Antwort an \(modelProviderName) gesendet.",
                 "Each request sends up to 30 recent chat messages, tab/note metadata, and selected email context to \(modelProviderName). When the agent reads email, web pages, or notes, the retrieved content is also sent to \(modelProviderName) to produce the response.")
    }

    private func footerHeader(_ title: String, icon: String, expanded: Bool) -> some View {
        HStack(spacing: 8) {
            Image(systemName: expanded ? "chevron.down" : "chevron.right")
                .font(.system(size: 9, weight: .semibold)).frame(width: 12)
            Image(systemName: icon).font(.system(size: 12)).frame(width: 15)
            Text(title).font(.system(size: 10, weight: .medium))
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, minHeight: 28, alignment: .leading)
        .contentShape(Rectangle())
    }
}

private struct AgentThinkingBubble: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(L("Agent denkt", "Agent is thinking"))
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(ink.opacity(0.62))
            TimelineView(.animation(minimumInterval: reduceMotion ? 1 : 1.0 / 30.0)) { context in
                let phase = reduceMotion ? 0 : context.date.timeIntervalSinceReferenceDate * 4.2
                HStack(spacing: 5) {
                    ForEach(0..<3, id: \.self) { index in
                        let wave = (sin(phase - Double(index) * 0.9) + 1) / 2
                        Circle()
                            .fill(moss.opacity(0.38 + wave * 0.62))
                            .frame(width: 6, height: 6)
                            .scaleEffect(0.72 + wave * 0.38)
                            .offset(y: reduceMotion ? 0 : -wave * 3)
                    }
                }
                .frame(height: 11, alignment: .bottom)
            }
        }
        .padding(.horizontal, 13)
        .padding(.vertical, 11)
        .background(YOBROTheme.surface.opacity(0.7), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(moss.opacity(0.12)))
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(L("Agent denkt und erstellt eine Antwort", "Agent is thinking and composing a response"))
    }
}

private struct ChatEntryBubble: View {
    let entry: SpaceChatEntry
    @ObservedObject var chat: SpaceChatStore
    @State private var copied = false
    @State private var copyResetTask: Task<Void, Never>?
    private var isUser: Bool { entry.kind == "user" }
    private var isContext: Bool { entry.kind == "context" }
    var body: some View {
        HStack {
            if isUser { Spacer(minLength: 42) }
            VStack(alignment: .leading, spacing: 7) {
                HStack(spacing: 6) {
                    if entry.kind == "assistant" {
                        YOBROMark(size: 10)
                            .colorMultiply(moss)
                            .frame(width: 10, height: 10)
                    } else {
                        Image(systemName: icon).font(.system(size: 9)).foregroundStyle(moss)
                    }
                    Text(label).font(.system(size: 9, weight: .semibold)).foregroundStyle(.secondary)
                    Spacer()
                    Text(entry.date, style: .time).font(.system(size: 8)).foregroundStyle(.tertiary)
                }
                if isContext {
                    DisclosureGroup(L("E-Mail-Kontext", "Email context")) { Text(entry.text).textSelection(.enabled).padding(.top, 4) }
                } else { Text(entry.text).textSelection(.enabled) }
                if entry.kind == "assistant" {
                    HStack {
                        Button {
                            NSPasteboard.general.clearContents()
                            copied = NSPasteboard.general.setString(entry.text, forType: .string)
                            copyResetTask?.cancel()
                            copyResetTask = Task { @MainActor in
                                try? await Task.sleep(nanoseconds: 1_500_000_000)
                                guard !Task.isCancelled else { return }
                                copied = false
                            }
                        } label: {
                            Image(systemName: copied ? "checkmark" : "doc.on.doc")
                                .font(.system(size: 11, weight: .semibold))
                                .foregroundStyle(copied ? moss : ink.opacity(0.62))
                                .frame(width: 28, height: 26)
                                .background(ink.opacity(0.055), in: RoundedRectangle(cornerRadius: 7))
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(copied ? L("Kopiert", "Copied") : L("Antwort kopieren", "Copy response"))
                        .accessibilityLabel(copied ? L("Kopiert", "Copied") : L("Antwort kopieren", "Copy response"))
                        Spacer()
                    }
                }
            }
            .font(.system(size: 12)).padding(11)
            .background(isUser ? moss.opacity(0.16) : YOBROTheme.surface.opacity(0.58), in: RoundedRectangle(cornerRadius: 13))
            if !isUser { Spacer(minLength: 18) }
        }
        .id(entry.id)
        .onDisappear { copyResetTask?.cancel() }
    }
    private var icon: String {
        switch entry.kind {
        case "user": return "person.fill"
        case "context": return "envelope.fill"
        case "error": return "exclamationmark.triangle.fill"
        default: return "bolt.fill"
        }
    }
    private var label: String {
        switch entry.kind {
        case "user": return L("DU", "YOU")
        case "assistant": return "YoBro"
        case "context": return "MAIL"
        case "error": return L("FEHLER", "ERROR")
        default: return L("AKTION", "ACTION")
        }
    }
}
