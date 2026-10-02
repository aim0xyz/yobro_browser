import SwiftUI
import AVFoundation

@MainActor
private final class BroSpeechPlayback: ObservableObject {
    private let synthesizer = AVSpeechSynthesizer()

    func speak(_ text: String) {
        synthesizer.stopSpeaking(at: .immediate)
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: YOBROLanguage.speechLanguage)
        synthesizer.speak(utterance)
    }
}

/// The browser start page is the assistant's local dashboard. It reuses the
/// existing chat and mail stores, including their account and Space boundaries.
struct BroDashboard: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var chat: SpaceChatStore
    @ObservedObject var mail: MailStore
    @ObservedObject var workspaces: BroWorkspaceStore
    @ObservedObject var voice: YoBroVoice
    @State private var prompt = ""
    /// Sender and subject lines are visible on every new tab, including while
    /// sharing the screen. Let the user keep them out of sight.
    @AppStorage("YOBRO.dashboardShowsMail") private var showsMail = true
    @StateObject private var speech = BroSpeechPlayback()

    private var recentMail: [MailItem] {
        Array(mail.items.filter { $0.folder.uppercased() == "INBOX" }.sorted { $0.date > $1.date }.prefix(8))
    }

    private var unreadCount: Int { mail.items.filter { $0.folder.uppercased() == "INBOX" && $0.unread }.count }

    private var latestAnswer: String? {
        chat.entries.last { $0.space == model.space && $0.kind == "assistant" }?.text
    }

    private var spaceTimers: [BroTimer] { chat.timers.filter { $0.space == model.space }.sorted { $0.due < $1.due } }

    var body: some View {
        VStack(spacing: 12) {
            agentCard
            mailCard
            workspaceCard
            if !spaceTimers.isEmpty {
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    HStack(spacing: 10) {
                        Image(systemName: "timer").foregroundStyle(moss)
                        ForEach(spaceTimers.prefix(3)) { timer in
                            let remaining = max(0, Int(ceil(timer.due.timeIntervalSince(context.date))))
                            Text(timer.label.isEmpty ? L("Timer", "Timer") : timer.label).lineLimit(1)
                            Text("\(remaining / 60):\(String(format: "%02d", remaining % 60))")
                                .monospacedDigit().foregroundStyle(moss)
                        }
                        Spacer(minLength: 0)
                    }
                    .font(.system(size: 10))
                    .padding(.horizontal, 12).frame(height: 34)
                    .background(YOBROTheme.surface.opacity(0.72), in: RoundedRectangle(cornerRadius: 10))
                }
            }
        }
        .frame(maxWidth: 820)
        .accessibilityIdentifier("bro-dashboard")
    }

    private var agentCard: some View {
        VStack(alignment: .leading, spacing: 15) {
            HStack(spacing: 8) {
                Image(systemName: "sparkles.rectangle.stack.fill")
                    .font(.system(size: 20)).foregroundStyle(moss)
                    .frame(width: 40, height: 40)
                    .background(moss.opacity(0.12), in: RoundedRectangle(cornerRadius: 12))
                VStack(alignment: .leading, spacing: 2) {
                    Text("YO BRO").font(.system(size: 16, weight: .bold, design: .rounded)).tracking(0.6)
                    Text(L("Dein Assistent für diesen Space", "Your assistant for this Space"))
                        .font(.system(size: 10)).foregroundStyle(.secondary)
                }
                Spacer()
                Label(voice.enabled ? voice.activationMode.title : model.agentEnabled ? L("Bereit", "Ready") : L("Pausiert", "Paused"),
                      systemImage: voice.enabled ? "waveform" : "circle.fill")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(model.agentEnabled ? moss : Color.orange)
                    .padding(.horizontal, 10).padding(.vertical, 7)
                    .background(moss.opacity(0.09), in: Capsule())
            }
            if let latestAnswer {
                HStack(alignment: .top, spacing: 6) {
                    Text(latestAnswer).font(.system(size: 11)).lineLimit(2)
                        .foregroundStyle(ink.opacity(0.8))
                    Spacer(minLength: 2)
                    Button { speech.speak(latestAnswer) } label: { Image(systemName: "speaker.wave.2") }
                        .buttonStyle(.plain).foregroundStyle(moss)
                        .accessibilityLabel(L("Antwort vorlesen", "Read answer aloud"))
                }
            }
            HStack(spacing: 9) {
                TextField(L("Frag Bro etwas …", "Ask Bro something…"), text: $prompt)
                    .textFieldStyle(.plain)
                    .font(.system(size: 13))
                    .onSubmit(send)
                    .accessibilityIdentifier("bro-dashboard-prompt")
                Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.system(size: 19)) }
                    .buttonStyle(.plain).foregroundStyle(moss)
                    .disabled(prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || chat.running)
                    .accessibilityLabel(L("An Bro senden", "Send to Bro"))
            }
            .padding(.horizontal, 13).frame(height: 43)
            .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
            HStack(spacing: 8) {
                quickPrompt(L("Posteingang zusammenfassen", "Summarize inbox"), symbol: "envelope.badge")
                quickPrompt(L("Was steht an?", "What's next?"), symbol: "sparkles")
                Spacer(minLength: 0)
                Button(L("Chat öffnen", "Open chat")) { model.showAgent = true }
                    .buttonStyle(.plain).font(.system(size: 10, weight: .semibold)).foregroundStyle(moss)
            }
        }
        .dashboardCard()
    }

    private func quickPrompt(_ title: String, symbol: String) -> some View {
        Button { send(title) } label: {
            Label(title, systemImage: symbol)
                .font(.system(size: 10, weight: .medium))
                .padding(.horizontal, 10).frame(height: 29)
                .background(moss.opacity(0.09), in: Capsule())
        }
        .buttonStyle(.plain).disabled(chat.running || !model.agentEnabled)
    }

    private var mailCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "tray.full.fill").foregroundStyle(moss)
                Text(L("Posteingang", "Inbox")).font(.system(size: 13, weight: .bold, design: .rounded))
                if !mail.accounts.isEmpty {
                    Text("\(unreadCount) " + L("ungelesen", "unread"))
                        .font(.system(size: 10, weight: .medium)).foregroundStyle(moss)
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(moss.opacity(0.09), in: Capsule())
                }
                Spacer()
                if !mail.accounts.isEmpty {
                    Button { showsMail.toggle() } label: { Image(systemName: showsMail ? "eye.slash" : "eye") }
                        .buttonStyle(.plain).foregroundStyle(moss)
                        .help(showsMail ? L("Nachrichten ausblenden", "Hide messages") : L("Nachrichten anzeigen", "Show messages"))
                        .accessibilityLabel(showsMail ? L("Nachrichten ausblenden", "Hide messages") : L("Nachrichten anzeigen", "Show messages"))
                }
                Button(mail.accounts.isEmpty ? L("Konto hinzufügen", "Add account") : L("Alle E-Mails", "All mail")) { model.presentMail() }
                    .buttonStyle(.plain).font(.system(size: 10, weight: .semibold)).foregroundStyle(moss)
            }
            .padding(.bottom, 10)
            if mail.accounts.isEmpty {
                Text(L("Verbinde deine Konten im vorhandenen YoBro-Mailbereich.", "Connect your accounts in YoBro Mail."))
                    .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true).padding(.vertical, 10)
            } else if !showsMail {
                Text(L("Nachrichten ausgeblendet.", "Messages hidden."))
                    .font(.system(size: 11)).foregroundStyle(.secondary).padding(.vertical, 10)
            } else if recentMail.isEmpty {
                Text(L("Keine Nachrichten im Posteingang.", "No messages in the inbox."))
                    .font(.system(size: 11)).foregroundStyle(.secondary).padding(.vertical, 10)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(recentMail) { item in
                            Button {
                                model.presentMail()
                                mail.selectedAccount = item.accountID
                                mail.selectedFolder = "INBOX"
                                mail.selectedID = item.id
                            } label: {
                                HStack(spacing: 10) {
                                    Circle().fill(item.unread ? moss : moss.opacity(0.14)).frame(width: 6, height: 6)
                                    Text(mailSender(item)).font(.system(size: 11, weight: item.unread ? .semibold : .regular))
                                        .lineLimit(1).frame(minWidth: 80, maxWidth: 140, alignment: .leading)
                                    Text(item.subject.isEmpty ? L("Ohne Betreff", "No subject") : item.subject)
                                        .font(.system(size: 11, weight: item.unread ? .semibold : .regular)).lineLimit(1)
                                    Spacer(minLength: 4)
                                    Text(mailDate(item.date)).font(.system(size: 10)).foregroundStyle(.secondary)
                                }
                                .frame(height: 37)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            if item.id != recentMail.last?.id { Divider().opacity(0.45) }
                        }
                    }
                }
                .frame(height: CGFloat(recentMail.count * 38))
            }
        }
        .dashboardCard()
    }

    private func mailSender(_ item: MailItem) -> String {
        let name = item.sender.components(separatedBy: "<").first?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return name.isEmpty ? item.sender : name
    }

    private func mailDate(_ date: Date) -> String {
        if Calendar.current.isDateInToday(date) { return date.formatted(date: .omitted, time: .shortened) }
        return date.formatted(.dateTime.day().month(.abbreviated))
    }

    private var workspaceCard: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack {
                Label(L("Arbeitsmaterial", "Working material"), systemImage: "folder.badge.gearshape")
                    .font(.system(size: 12, weight: .semibold))
                Spacer()
                Button(L("Ordner hinzufügen", "Add folder")) { workspaces.chooseFolder() }
                    .buttonStyle(.bordered).controlSize(.small)
            }
            if workspaces.folders.isEmpty {
                Text(L("Gib Bro Zugriff auf einen Projektordner mit Anleitungen, Skills oder CLI-Dokumentation.",
                       "Give Bro access to a project folder with instructions, skills or CLI documentation."))
                    .font(.system(size: 10)).foregroundStyle(.secondary)
            } else {
                ForEach(workspaces.folders) { folder in
                    HStack(spacing: 7) {
                        Image(systemName: "folder.fill").foregroundStyle(moss)
                        Text(folder.name).fontWeight(.medium)
                        Text(folder.path).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
                        Spacer(minLength: 2)
                        Button { do { try workspaces.remove(folder.id) } catch { workspaces.error = error.localizedDescription } } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(L("Ordnerzugriff entfernen", "Remove folder access"))
                    }
                    .font(.system(size: 10))
                }
            }
            if let error = workspaces.error {
                Text(error).font(.system(size: 10)).foregroundStyle(.orange)
            }
            Text(L("Gelesene Dateien werden als Kontext an dein verbundenes Agentenmodell gesendet.",
                   "Files Bro reads are sent as context to your connected agent model."))
                .font(.system(size: 9)).foregroundStyle(.secondary)
        }
        .dashboardCard()
    }

    private func send() {
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        send(text)
    }

    private func send(_ text: String) {
        guard !text.isEmpty, !chat.running else { return }
        model.showAgent = true
        prompt = ""
        chat.send(text, browser: model)
    }
}

private extension View {
    func dashboardCard() -> some View {
        self.padding(15)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .background(YOBROTheme.surface.opacity(0.72), in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(YOBROTheme.border.opacity(0.45)))
    }
}
