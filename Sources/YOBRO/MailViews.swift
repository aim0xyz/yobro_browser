import SwiftUI
import AppKit

struct MailWorkspace: View {
    @ObservedObject var store: MailStore
    let close: () -> Void
    var openURL: (URL) -> Void = { NSWorkspace.shared.open($0) }
    var askAgent: ((MailItem, MailBody, String) -> Void)? = nil
    @State private var showAccount = false
    @State private var editing: MailAccount?
    @State private var compose = false
    @State private var remove: MailAccount?
    @State private var bodyError: String?
    @State private var plainText = false
    @State private var selectionMode = false
    @State private var markedIDs: Set<String> = []
    @State private var confirmDelete = false
    @State private var mailboxesExpanded = false

    var body: some View {
        GeometryReader { geometry in
        let compactMail = geometry.size.width < 880
        let singleColumnMail = geometry.size.width < 650
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                if singleColumnMail, store.selected != nil {
                    Button { store.selectedID = nil } label: { Image(systemName: "chevron.left") }
                        .help(L("Zurück zu Nachrichten", "Back to messages"))
                }
                Image(systemName: "envelope.open").foregroundStyle(moss)
                Text("YoBro Mail").font(.system(size: compactMail ? 17 : 21, weight: .medium, design: .serif)).lineLimit(1)
                Spacer()
                if compactMail {
                    Button { mailboxesExpanded.toggle() } label: { Image(systemName: "sidebar.left") }
                        .help(mailboxesExpanded ? L("Postfächer einklappen", "Collapse mailboxes") : L("Postfächer einblenden", "Show mailboxes"))
                        .accessibilityLabel(mailboxesExpanded ? L("Postfächer einklappen", "Collapse mailboxes") : L("Postfächer einblenden", "Show mailboxes"))
                }
                if store.refreshing { ProgressView().controlSize(.small) }
                Button { MailSecrets.session.retryFailures(); Task { await store.refresh() } } label: { Image(systemName: "arrow.clockwise") }.disabled(store.accounts.isEmpty || store.refreshing).help(L("Alle Postfächer aktualisieren"))
                if compactMail {
                    Button { compose = true } label: { Image(systemName: "square.and.pencil") }.disabled(store.accounts.isEmpty).help(L("Schreiben"))
                    Menu {
                        Button(store.notificationsEnabled ? L("Mail-Mitteilungen ausschalten") : L("Mail-Mitteilungen einschalten")) { Task { await store.setNotifications(!store.notificationsEnabled) } }
                        Button(L("Postfach verbinden")) { editing = nil; showAccount = true }
                    } label: { Image(systemName: "ellipsis") }
                } else {
                    Button { Task { await store.setNotifications(!store.notificationsEnabled) } } label: { Image(systemName: store.notificationsEnabled ? "bell.badge.fill" : "bell.slash") }.help(store.notificationsEnabled ? L("Mail-Mitteilungen ausschalten") : L("Mail-Mitteilungen einschalten"))
                    Button { editing = nil; showAccount = true } label: { Image(systemName: "person.badge.plus") }.help(L("Postfach verbinden"))
                    Button { compose = true } label: {
                        Label(L("Schreiben"), systemImage: "square.and.pencil")
                            .padding(.horizontal, 11)
                    }
                    .disabled(store.accounts.isEmpty)
                    .help(L("Neue E-Mail verfassen", "Compose a new email"))
                }
                Button(action: close) { Image(systemName: "xmark") }.help(L("Zurück zum Browser"))
            }.buttonStyle(YOBROButtonStyle()).padding(.horizontal, 16).frame(height: 52)
            Divider().opacity(0.4)
            ForEach(store.accounts.filter { store.lockedAccounts.contains($0.id) }) { account in
                HStack {
                    Label(L("\(account.label.isEmpty ? account.address : account.label): Mail-Zugriff freigeben", "\(account.label.isEmpty ? account.address : account.label): Allow mail access"), systemImage: "lock.fill").font(.system(size: 12))
                    Spacer()
                    Button(L("Mail entsperren")) { Task { await store.unlockMail(account.id) } }
                        .disabled(store.refreshing || store.unlockingAccount != nil)
                }.padding(12).background(moss.opacity(0.1))
            }
            if store.accounts.isEmpty { ScrollView { welcome.frame(minHeight: 660) } }
            else {
                HStack(spacing: 0) {
                    if !compactMail || mailboxesExpanded {
                    VStack(alignment: .leading, spacing: 12) {
                        Text(L("POSTFÄCHER")).font(.system(size: 9, weight: .semibold)).tracking(1.5).foregroundStyle(ink.opacity(0.45))
                        accountButton(L("Alle Eingänge"), icon: "tray.2", id: nil)
                        ScrollView {
                        VStack(alignment: .leading, spacing: 3) {
                        ForEach(store.accounts) { account in
                            accountButton(account.label.isEmpty ? account.address : account.label, icon: "envelope", id: account.id)
                                .contextMenu {
                                    Button(L("Konto bearbeiten")) { editing = account; showAccount = true }
                                    Button(L("Konto entfernen"), role: .destructive) { remove = account }
                                }
                            if store.selectedAccount == account.id {
                                ForEach(store.folders[account.id] ?? []) { folder in
                                    Button { store.selectFolder(account: account.id, folder: folder.path) } label: {
                                        HStack(spacing: 6) {
                                            Image(systemName: folder.path.uppercased() == "INBOX" ? "tray" : "folder").foregroundStyle(moss)
                                            Text(folder.displayTitle).lineLimit(2)
                                            Spacer(minLength: 0)
                                            if folder.unread > 0 { MailUnreadDot() }
                                        }.font(.system(size: 10)).padding(.horizontal, 9).padding(.vertical, 5)
                                            .frame(maxWidth: .infinity, alignment: .leading)
                                            .background(store.selectedFolder == folder.path ? moss.opacity(0.12) : .clear, in: RoundedRectangle(cornerRadius: 7))
                                    }.buttonStyle(YOBROButtonStyle(minimumSize: 30)).disabled(!folder.selectable).help(folder.displayTitle)
                                }.padding(.leading, 8)
                            }
                        }
                        }
                        }
                        Divider().padding(.vertical, 7)
                        Toggle(L("Nur ungelesen"), isOn: $store.unreadOnly).toggleStyle(.checkbox).font(.system(size: 11))
                        Spacer()
                        Text(L("Automatische Aktualisierung jede Minute. Ordner zeigen zunächst 100 Nachrichten."))
                            .font(.system(size: 10)).foregroundStyle(ink.opacity(0.45)).lineSpacing(4)
                        if let date = store.lastRefresh { Text(date, style: .time).font(.system(size: 10)).foregroundStyle(moss) }
                    }.padding(12).frame(width: 190).background(moss.opacity(0.035))
                    Divider().opacity(0.4)
                    }
                    if singleColumnMail {
                        if store.selected != nil { reader.frame(maxWidth: .infinity, maxHeight: .infinity) }
                        else { messageList.frame(maxWidth: .infinity) }
                    } else {
                        messageList.frame(minWidth: 220, idealWidth: 295, maxWidth: 320)
                        Divider().opacity(0.4)
                        reader.frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
        }
        .onChange(of: compactMail) { _, compact in if compact { mailboxesExpanded = false } }
        }
        .background(paper).foregroundStyle(ink)
            .sheet(isPresented: $showAccount) { MailAccountEditor(store: store, existing: editing) }
            .sheet(isPresented: $compose) { MailComposer(store: store) }
            .overlay {
                if remove != nil {
                    YOBRODialogOverlay(icon: "person.crop.circle.badge.minus", title: L("Postfach entfernen?"), message: L("Das Konto und sein Passwort werden aus YoBro entfernt. Nachrichten beim Anbieter bleiben erhalten."), confirmTitle: L("Entfernen"), cancelTitle: L("Abbrechen"), destructive: true, confirm: {
                        if let account = remove { do { try store.disconnect(account) } catch { store.notice = error.localizedDescription } }
                        remove = nil
                    }, cancel: { remove = nil })
                } else if confirmDelete {
                    YOBRODialogOverlay(icon: "trash.fill", title: deleteTitle, message: L("Diese Aktion wird direkt mit deinem Mailserver synchronisiert.", "This action is synchronized directly with your mail server."), confirmTitle: deleteTitle, cancelTitle: L("Abbrechen", "Cancel"), destructive: true, confirm: { confirmDelete = false; performDelete() }, cancel: { confirmDelete = false })
                } else if let notice = store.notice {
                    YOBRODialogOverlay(icon: "paperplane.fill", title: "YoBro Mail", message: notice, confirmTitle: "OK", confirm: { store.notice = nil })
                }
            }
            .onChange(of: store.selectedAccount) { _, _ in endSelection() }
            .onChange(of: store.selectedFolder) { _, _ in endSelection() }
            .task {
                if ProcessInfo.processInfo.environment["YOBRO_DEV_CAPTURE"] == "1", ProcessInfo.processInfo.environment["YOBRO_MAIL_SETUP_PREVIEW"] != nil { showAccount = true }
                if !store.accounts.isEmpty { await store.refresh() }
            }
    }
    private var welcome: some View {
        VStack(spacing: 20) {
            Spacer()
            Image(systemName: "tray.2").font(.system(size: 55, weight: .ultraLight)).foregroundStyle(moss).padding(26).background(moss.opacity(0.07), in: Circle())
            Text(L("Alle Postfächer.\nEin ruhiger Ort.")).font(.system(size: 39, design: .serif)).multilineTextAlignment(.center)
            Text(L("Deine E-Mails, direkt in YoBro. Verbinde deine Konten,\nlese Nachrichten und antworte aus einem gemeinsamen Eingang."))
                .font(.system(size: 13)).foregroundStyle(ink.opacity(0.6)).multilineTextAlignment(.center).lineSpacing(5)
            Button { editing = nil; showAccount = true } label: { Label(L("Erstes Postfach verbinden"), systemImage: "plus").padding(.horizontal, 12).padding(.vertical, 5) }.buttonStyle(.borderedProminent).tint(moss)
            HStack(spacing: 18) { Text("Gmail"); Text("iCloud Mail"); Text("Spacemail"); Text("IMAP / SMTP") }.font(.system(size: 11)).foregroundStyle(ink.opacity(0.45)).padding(.top, 10)
            Text(L("App-Passwort oder Postfachpasswort · Je nach Anbieter und Kontoeinstellungen")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.4))
            Spacer()
            Label(L("Direkte Verbindung zum Anbieter · Passwörter im Schlüsselbund"), systemImage: "lock.shield").font(.system(size: 10)).foregroundStyle(moss).padding(.bottom, 25)
        }.frame(maxWidth: .infinity)
    }
    private func accountButton(_ title: String, icon: String, id: UUID?) -> some View {
        Button { store.selectFolder(account: id) } label: {
            HStack(spacing: 8) {
                Image(systemName: icon).frame(width: 17)
                Text(title).lineLimit(1)
                Spacer(minLength: 0)
                if id.map({ store.hasUnread($0) }) ?? store.hasUnread { MailUnreadDot() }
                if let id, store.errors[id] != nil { Image(systemName: "exclamationmark.circle").foregroundStyle(.orange) }
            }.font(.system(size: 11, weight: .medium)).padding(10).background(store.selectedAccount == id ? moss.opacity(0.10) : .clear, in: RoundedRectangle(cornerRadius: 9))
        }.buttonStyle(YOBROButtonStyle())
    }
    private var messageList: some View {
        VStack(spacing: 0) {
            HStack {
                Image(systemName: "magnifyingglass")
                TextField(L("Nachrichten suchen"), text: $store.search).textFieldStyle(.plain)
                Button(selectionMode ? L("Fertig", "Done") : L("Auswählen", "Select")) {
                    if selectionMode { endSelection() } else { selectionMode = true }
                }.buttonStyle(.plain).foregroundStyle(moss)
            }.font(.system(size: 12)).padding(15)
            if selectionMode {
                HStack {
                    Button(markedIDs.count == store.filtered.count && !store.filtered.isEmpty ? L("Auswahl aufheben", "Deselect all") : L("Alle", "All")) {
                        if markedIDs.count == store.filtered.count { markedIDs.removeAll() }
                        else { markedIDs = Set(store.filtered.map(\.id)) }
                    }
                    Spacer()
                    Text(L("\(markedIDs.count) ausgewählt", "\(markedIDs.count) selected")).foregroundStyle(.secondary)
                }.buttonStyle(.plain).font(.system(size: 10)).padding(.horizontal, 15).padding(.bottom, 8)
            }
            ForEach(store.accounts.filter { store.errors[$0.id] != nil && (store.selectedAccount == nil || store.selectedAccount == $0.id) }) { account in
                VStack(alignment: .leading, spacing: 5) {
                    Text(account.address).fontWeight(.medium)
                    Text(store.errors[account.id] ?? "")
                    Button(L("Kontodaten prüfen")) { editing = account; showAccount = true }.buttonStyle(YOBROButtonStyle()).foregroundStyle(moss)
                }.font(.system(size: 10)).foregroundStyle(.orange).padding(12).frame(maxWidth: .infinity, alignment: .leading)
            }
            ScrollView {
                LazyVStack(spacing: 2) {
                    if store.filtered.isEmpty {
                        Text(store.refreshing ? L("Nachrichten werden geladen …") : L("Keine Nachrichten in dieser Ansicht.")).font(.system(size: 12)).foregroundStyle(ink.opacity(0.45)).padding(25)
                    }
                    ForEach(store.filtered) { item in
                        Button {
                            if selectionMode {
                                if markedIDs.contains(item.id) { markedIDs.remove(item.id) } else { markedIDs.insert(item.id) }
                            } else { store.selectedID = item.id }
                        } label: {
                            HStack(spacing: 9) {
                                if selectionMode {
                                    Image(systemName: markedIDs.contains(item.id) ? "checkmark.circle.fill" : "circle")
                                        .foregroundStyle(markedIDs.contains(item.id) ? moss : ink.opacity(0.35))
                                }
                                VStack(alignment: .leading, spacing: 8) {
                                HStack {
                                    MailUnreadDot().opacity(item.unread ? 1 : 0)
                                    Text(item.sender).font(.system(size: 11, weight: .semibold)).lineLimit(1)
                                    Spacer()
                                    Text(item.date, format: .dateTime.day().month()).font(.system(size: 9)).foregroundStyle(ink.opacity(0.4))
                                }
                                Text(item.subject.isEmpty ? L("(Ohne Betreff)") : item.subject).font(.system(size: 12)).lineLimit(2)
                                Text(store.accounts.first(where: { $0.id == item.accountID })?.address ?? "").font(.system(size: 9)).foregroundStyle(moss).lineLimit(1)
                                }
                            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
                                .background(markedIDs.contains(item.id) || store.selectedID == item.id ? moss.opacity(0.10) : YOBROTheme.surface.opacity(0.25), in: RoundedRectangle(cornerRadius: 9))
                        }.buttonStyle(YOBROButtonStyle())
                    }
                    if let account = store.selectedAccount, (store.folderTotals["\(account):\(store.selectedFolder)"] ?? 0) > store.filtered.count {
                        Button(L("Ältere Nachrichten laden")) { Task { await store.loadMore() } }.disabled(store.folderLoading)
                    }
                }.padding(.horizontal, 8)
            }
            if store.folderLoading { ProgressView().controlSize(.small).padding(8) }
            if selectionMode {
                Divider().opacity(0.4)
                HStack {
                    if let account = store.selectedAccount {
                        Menu {
                            ForEach((store.folders[account] ?? []).filter { $0.selectable && $0.path != store.selectedFolder }) { folder in
                                Button(folder.displayTitle) { performMove(to: folder.path) }
                            }
                        } label: { Label(L("Verschieben", "Move"), systemImage: "folder") }
                        .disabled(markedIDs.isEmpty || store.organizing)
                    }
                    Spacer()
                    if store.organizing { ProgressView().controlSize(.small) }
                    Button(role: .destructive) { confirmDelete = true } label: { Label(deleteTitle, systemImage: "trash") }
                        .disabled(markedIDs.isEmpty || store.organizing)
                }.buttonStyle(.bordered).font(.system(size: 11)).padding(10)
            }
        }
    }

    private var markedItems: [MailItem] { store.items.filter { markedIDs.contains($0.id) } }
    private var currentFolderIsTrash: Bool {
        guard let account = store.selectedAccount else { return false }
        return store.folders[account]?.first(where: { $0.path == store.selectedFolder })?.isTrash == true
    }
    private var deleteTitle: String {
        currentFolderIsTrash ? L("Endgültig löschen", "Delete permanently") : L("In Papierkorb", "Move to Trash")
    }
    private func endSelection() { selectionMode = false; markedIDs.removeAll() }
    private func performDelete() {
        let messages = markedItems
        Task {
            do { try await store.delete(messages); endSelection() }
            catch { store.notice = error.localizedDescription }
        }
    }
    private func performMove(to folder: String) {
        let messages = markedItems
        Task {
            do { try await store.move(messages, to: folder); endSelection() }
            catch { store.notice = error.localizedDescription }
        }
    }
    private var reader: some View {
        Group {
            if let item = store.selected {
                VStack(alignment: .leading, spacing: 12) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(item.subject.isEmpty ? L("(Ohne Betreff)") : item.subject).font(.system(size: 22, weight: .medium, design: .serif)).lineLimit(3).textSelection(.enabled)
                        Text(L("Von: ") + item.sender).font(.system(size: 11)).textSelection(.enabled)
                        Text(L("An: ") + item.recipient).font(.system(size: 10)).foregroundStyle(.secondary).textSelection(.enabled)
                        HStack {
                            Text(item.date, format: .dateTime.day().month().year().hour().minute()).font(.system(size: 10)).foregroundStyle(.secondary)
                            Spacer()
                            if let askAgent, let body = store.bodies[item.id] {
                                Menu {
                                    ForEach([L("Fasse diese E-Mail zusammen", "Summarize this email"), L("Öffne alle genannten Links", "Open all mentioned links"), L("Prüfe die genannten Preise", "Check the mentioned prices"), L("Entwirf eine Antwort", "Draft a reply")], id: \.self) { prompt in
                                        Button(prompt) { askAgent(item, body, prompt) }
                                    }
                                } label: { Label(L("Im Space bearbeiten", "Work in Space"), systemImage: "sparkles") }
                            }
                            Button { store.reply(item); compose = true } label: { Label(L("Antworten"), systemImage: "arrowshape.turn.up.left") }.buttonStyle(.bordered)
                        }
                    }.padding(.horizontal, 20).padding(.top, 16)
                    if let body = store.bodies[item.id] {
                        if !body.html.isEmpty {
                            HStack {
                                Toggle(L("Nur Text"), isOn: $plainText).toggleStyle(.checkbox)
                                Spacer()
                                if !plainText {
                                    Toggle(L("Externe Bilder laden"), isOn: Binding(
                                        get: { store.remoteImagesEnabled },
                                        set: { store.setRemoteImages($0) }
                                    )).toggleStyle(.checkbox).help(L("Kann dem Absender das Öffnen der Nachricht melden."))
                                }
                            }.font(.system(size: 10)).padding(.horizontal, 20)
                        }
                        if !body.attachments.isEmpty {
                            ScrollView(.horizontal) {
                                HStack {
                                    ForEach(body.attachments) { attachment in
                                        Button { store.download(attachment, from: item) } label: {
                                            HStack(spacing: 8) {
                                                Image(systemName: "arrow.down.doc")
                                                VStack(alignment: .leading) {
                                                    Text(attachment.name).lineLimit(1)
                                                    Text(ByteCountFormatter.string(fromByteCount: Int64(attachment.size), countStyle: .file)).foregroundStyle(.secondary)
                                                }
                                            }.font(.system(size: 10)).padding(9).background(moss.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                                        }.buttonStyle(YOBROButtonStyle()).disabled(store.attachmentDownloads.contains(item.id + ":" + attachment.id)).help(L("Anhang speichern: ") + attachment.name)
                                    }
                                }.padding(.horizontal, 20)
                            }
                        }
                        if !body.html.isEmpty && !plainText {
                            MailHTMLView(html: body.html, remoteImages: store.remoteImagesEnabled, openURL: openURL).frame(maxWidth: .infinity, maxHeight: .infinity)
                        } else {
                            ScrollView {
                                Text(body.text.isEmpty ? (body.attachments.isEmpty ? L("Diese Nachricht enthält keinen darstellbaren Text.") : L("Der Inhalt befindet sich in den Anhängen.")) : body.text)
                                    .font(.system(size: 13)).lineSpacing(5).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(20)
                            }.frame(maxHeight: .infinity)
                        }
                    } else if let error = bodyError {
                        Text(error).font(.system(size: 12)).foregroundStyle(.orange).padding(20)
                        Button(L("Erneut laden")) { Task { bodyError = nil; do { try await store.load(item) } catch { bodyError = error.localizedDescription } } }.padding(.horizontal, 20)
                        Spacer()
                    } else { ProgressView(L("Nachricht laden …")).padding(20); Spacer() }
                }.task(id: item.id) {
                    bodyError = nil; plainText = false
                    do { try await store.load(item) } catch { if store.selectedID == item.id { bodyError = error.localizedDescription } }
                }
            } else {
                VStack(spacing: 15) {
                    Image(systemName: "envelope.open").font(.system(size: 38, weight: .ultraLight)).foregroundStyle(moss)
                    Text(L("Platz für deine Nachrichten.")).font(.system(size: 24, design: .serif))
                    Text(L("Wähle links eine E-Mail aus.")).font(.system(size: 12)).foregroundStyle(ink.opacity(0.45))
                }
            }
        }
    }
}

struct MailAccountEditor: View {
    @ObservedObject var store: MailStore
    let existing: MailAccount?
    @Environment(\.dismiss) private var dismiss
    @State private var account = MailAccount()
    @State private var password = ""
    @State private var provider = "Automatisch"
    @State private var explanation = L("YoBro erkennt die Server anhand deiner E-Mail-Adresse.")
    @State private var source = ""
    @State private var detecting = false
    @State private var oauthOnly = false
    @State private var expanded = false
    @State private var error: String?
    @State private var lookup: Task<Void, Never>?
    @State private var revision = UUID()
    @FocusState private var focused: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: "envelope.badge.shield.half.filled")
                    .font(.system(size: 22, weight: .light)).foregroundStyle(moss)
                    .frame(width: 46, height: 46).background(moss.opacity(0.09), in: RoundedRectangle(cornerRadius: 14))
                VStack(alignment: .leading, spacing: 5) {
                    Text(existing == nil ? L("Dein Postfach in YoBro.") : L("Postfach bearbeiten"))
                        .font(.system(size: 25, design: .serif))
                    Text(L("Alle Nachrichten. Ein Ort.")).font(.system(size: 12)).foregroundStyle(ink.opacity(0.5))
                }
                Spacer()
                Button { dismiss() } label: {
                    Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
                        .frame(width: 26, height: 26).background(ink.opacity(0.055), in: Circle())
                }.buttonStyle(YOBROButtonStyle()).help(L("Schließen")).keyboardShortcut(.cancelAction).disabled(store.connecting)
            }.padding(26)
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    field(L("E-Mail-Adresse")) {
                        TextField(L("du@beispiel.de", "you@example.com"), text: $account.address).focused($focused, equals: "email")
                            .onSubmit { schedule(immediate: true) }
                    }
                    HStack {
                        Text(L("Anbieter")).foregroundStyle(ink.opacity(0.55))
                        Spacer()
                        Picker(L("Anbieter"), selection: $provider) {
                            ForEach(["Automatisch", "Gmail", "iCloud Mail", "Spacemail", "Outlook / Microsoft 365", "Manuell"], id: \.self) { Text(L($0)).tag($0) }
                        }.labelsHidden().fixedSize().accessibilityLabel(L("Mail-Anbieter"))
                    }.font(.system(size: 12))
                    field(L("Passwort")) {
                        SecureField(L("App-Passwort oder Postfachpasswort"), text: $password).disabled(oauthOnly)
                    }
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: oauthOnly ? "info.circle" : "key.horizontal").foregroundStyle(moss)
                        Text(explanation).foregroundStyle(ink.opacity(0.55)).fixedSize(horizontal: false, vertical: true)
                    }.font(.system(size: 11)).lineSpacing(3)
                    if let help = MailPasswordProvider.detect(provider: provider, account: account) {
                        MailPasswordHelp(provider: help)
                    }
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Button { expanded.toggle() } label: {
                                HStack(spacing: 8) {
                                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0)).font(.system(size: 9, weight: .semibold))
                                    Text(L("Server & Kontodetails")).font(.system(size: 12, weight: .medium))
                                }.contentShape(Rectangle())
                            }.buttonStyle(YOBROButtonStyle()).accessibilityValue(expanded ? L("Ausgeklappt") : L("Eingeklappt"))
                            Spacer()
                            if detecting { ProgressView().controlSize(.mini) }
                            else if !account.imapHost.isEmpty { Label("TLS", systemImage: "lock.fill").font(.system(size: 10)).foregroundStyle(moss) }
                        }
                        if !expanded {
                            Text(detecting ? L("Server werden gesucht …") : account.imapHost.isEmpty ? L("Werden automatisch eingerichtet") : "IMAP  \(account.imapHost):\(account.imapPort)\nSMTP  \(account.smtpHost):\(account.smtpPort)")
                                .font(.system(size: 10)).foregroundStyle(ink.opacity(0.5)).lineSpacing(4).textSelection(.enabled)
                        } else {
                            field(L("Kontoname")) { TextField(L("Mein Postfach"), text: $account.label) }
                            field(L("Benutzername")) { TextField(account.address, text: $account.username) }
                            HStack(alignment: .top, spacing: 10) {
                                field("IMAP-Server") { TextField(L("imap.beispiel.de", "imap.example.com"), text: manual(\.imapHost)) }
                                field("Port") { TextField("993", value: manual(\.imapPort), format: .number.grouping(.never)) }.frame(width: 70)
                            }
                            HStack(alignment: .top, spacing: 10) {
                                field("SMTP-Server") { TextField(L("smtp.beispiel.de", "smtp.example.com"), text: manual(\.smtpHost)) }
                                field("Port") { TextField("465", value: manual(\.smtpPort), format: .number.grouping(.never)) }.frame(width: 70)
                            }
                            field(L("SMTP-Benutzername · optional")) {
                                TextField(L("Wie Benutzername"), text: Binding(get: { account.smtpUsername ?? "" }, set: { account.smtpUsername = $0 }))
                            }
                            Picker(L("SMTP-Sicherheit"), selection: manual(\.smtpSecurity)) { Text(L("TLS direkt")).tag("tls"); Text("STARTTLS").tag("starttls") }.font(.system(size: 11))
                            if !source.isEmpty { Text(source).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45)) }
                            Button(L("Server erneut erkennen")) { provider = "Automatisch"; schedule(immediate: true) }.buttonStyle(YOBROButtonStyle()).foregroundStyle(moss).font(.system(size: 11)).disabled(detecting || account.address.isEmpty)
                            Text(L("Für die Erkennung wird nur die Domain bei deinem Anbieter und gegebenenfalls der Thunderbird-Anbieterdatenbank abgefragt.")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45)).fixedSize(horizontal: false, vertical: true)
                        }
                    }.padding(14).background(moss.opacity(0.045), in: RoundedRectangle(cornerRadius: 12))
                    if let error {
                        Label(error, systemImage: "exclamationmark.circle").font(.system(size: 11)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                    }
                }.padding(.horizontal, 26).padding(.bottom, 22).disabled(store.connecting)
            }.frame(height: 390)
            Divider().opacity(0.35)
            HStack(spacing: 16) {
                Label(L("Sicher im\nmacOS-Schlüsselbund"), systemImage: "lock.shield")
                    .font(.system(size: 10)).foregroundStyle(ink.opacity(0.5)).lineSpacing(3)
                Spacer()
                Button(action: connect) {
                    HStack(spacing: 8) {
                        if store.connecting { ProgressView().controlSize(.small) }
                        Text(store.connecting ? L("Wird verbunden …") : L("Postfach verbinden"))
                        if !store.connecting { Image(systemName: "arrow.right") }
                    }.font(.system(size: 12, weight: .medium)).padding(.horizontal, 8).padding(.vertical, 6)
                }.buttonStyle(.borderedProminent).tint(moss)
                    .disabled(store.connecting || detecting || oauthOnly || password.isEmpty || account.address.isEmpty || account.imapHost.isEmpty || account.smtpHost.isEmpty)
            }.padding(.horizontal, 26).padding(.vertical, 19)
        }.frame(width: 540).background(paper).foregroundStyle(ink).interactiveDismissDisabled(store.connecting)
            .onAppear {
                if let existing { account = existing; provider = "Manuell"; expanded = true }
                if ProcessInfo.processInfo.environment["YOBRO_DEV_CAPTURE"] == "1", ProcessInfo.processInfo.environment["YOBRO_MAIL_SETUP_PREVIEW"] == "expanded" { expanded = true }
                focused = "email"
            }
            .onChange(of: account.address) { old, new in
                if account.username == old { account.username = new }
                if account.smtpUsername == old { account.smtpUsername = new }
                schedule()
            }
            .onChange(of: provider) { _, _ in schedule() }
            .onDisappear { lookup?.cancel(); revision = UUID() }
    }

    private func field<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title).font(.system(size: 11, weight: .medium)).foregroundStyle(ink.opacity(0.65))
            content().textFieldStyle(.plain).font(.system(size: 13)).padding(.horizontal, 11).frame(height: 36)
                .background(YOBROTheme.surface.opacity(0.7), in: RoundedRectangle(cornerRadius: 8))
                .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(ink.opacity(0.1), lineWidth: 1))
                .accessibilityLabel(title)
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
    private func manual<T>(_ path: WritableKeyPath<MailAccount, T>) -> Binding<T> {
        Binding(get: { account[keyPath: path] }, set: { value in
            lookup?.cancel(); revision = UUID(); detecting = false; provider = "Manuell"
            account[keyPath: path] = value
        })
    }
    private func schedule(immediate: Bool = false) {
        lookup?.cancel(); revision = UUID(); detecting = false; error = nil
        let token = revision
        if provider == "Manuell" {
            oauthOnly = false; explanation = L("Verwende das Postfachpasswort oder ein App-Passwort deines Anbieters."); source = L("Manuelle Konfiguration"); return
        }
        if provider != "Automatisch" { apply(MailDiscovery.preset(provider, address: account.address, id: account.id)); return }
        account.imapHost = ""; account.smtpHost = ""; source = ""; oauthOnly = false
        explanation = L("YoBro erkennt die Server anhand deiner E-Mail-Adresse.")
        let address = account.address.trimmingCharacters(in: .whitespacesAndNewlines)
        guard address.contains("@"), address.split(separator: "@").last?.contains(".") == true else { return }
        detecting = true
        lookup = Task {
            defer { if revision == token { detecting = false } }
            do {
                if !immediate { try await Task.sleep(nanoseconds: 600_000_000) }
                let result = try await MailDiscovery.discover(address: address, id: account.id)
                guard !Task.isCancelled, revision == token else { return }
                apply(result)
            } catch {
                guard !Task.isCancelled, revision == token else { return }
                self.error = error.localizedDescription; expanded = true
            }
        }
    }
    private func apply(_ result: MailDiscoveryResult) {
        account.imapHost = result.account.imapHost; account.imapPort = result.account.imapPort
        account.smtpHost = result.account.smtpHost; account.smtpPort = result.account.smtpPort; account.smtpSecurity = result.account.smtpSecurity
        if account.username.isEmpty || account.username == account.address { account.username = result.account.username }
        if account.smtpUsername == nil || account.smtpUsername == account.address { account.smtpUsername = result.account.smtpUsername }
        explanation = result.explanation; oauthOnly = result.oauthOnly; source = result.source; error = nil
    }
    private func connect() {
        Task {
            error = nil
            do {
                var submitted = account
                submitted.address = submitted.address.trimmingCharacters(in: .whitespacesAndNewlines)
                if account.username.isEmpty { account.username = account.address }
                if account.label.isEmpty { account.label = account.address }
                submitted.username = account.username; submitted.label = account.label
                try await store.connect(submitted, password: password)
                password = ""; dismiss(); await store.refresh()
            } catch { self.error = error.localizedDescription }
        }
    }
}

struct MailComposer: View {
    @ObservedObject var store: MailStore
    @Environment(\.dismiss) private var dismiss
    @State private var error: String?
    @StateObject private var commands = RichTextCommands()
    var body: some View {
        VStack(alignment: .leading, spacing: 17) {
            HStack {
                Text(L("Eine neue Nachricht.")).font(.system(size: 27, design: .serif))
                Spacer(); Button(L("Schließen")) { dismiss() }.disabled(store.sending)
            }
            Picker(L("Von"), selection: $store.draftAccount) { ForEach(store.accounts) { Text($0.address).tag(Optional($0.id)) } }
            TextField(L("An: name@beispiel.de"), text: $store.draftTo).textFieldStyle(YOBROTextFieldStyle())
            TextField(L("Betreff"), text: $store.draftSubject).textFieldStyle(YOBROTextFieldStyle())
            VStack(spacing: 0) {
                RichTextToolbar(commands: commands).padding(.horizontal, 10).frame(height: 40)
                Divider().opacity(0.35)
                RichTextEditor(rtf: store.draftRTF, fallbackText: store.draftText, commands: commands) { rtf, text in
                    store.draftRTF = rtf
                    store.draftText = text
                }
            }
            .background(YOBROTheme.surface.opacity(0.5), in: RoundedRectangle(cornerRadius: 10))
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .frame(height: 320)
            if let error { Text(error).font(.system(size: 12)).foregroundStyle(.orange) }
            HStack {
                Text(L("Entwurf bleibt bis zum Beenden von YoBro erhalten.")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
                Spacer()
                if store.sending { ProgressView().controlSize(.small) }
                Button { Task { error = nil; do { try await store.send(); dismiss() } catch { self.error = error.localizedDescription } } } label: { Label(L("Senden"), systemImage: "paperplane") }
                    .buttonStyle(.borderedProminent).tint(moss).disabled(store.sending || store.draftTo.isEmpty || store.draftText.isEmpty)
            }
            Text(L("Versand über dein gewähltes Konto. Nach erfolgreicher Annahme wird eine Kopie im Gesendet-Ordner des Anbieters gespeichert.")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
        }.padding(28).frame(width: 650)
            .background(YOBROArrowCursorRegion().ignoresSafeArea())
            .background(paper).foregroundStyle(ink).disabled(store.sending).interactiveDismissDisabled(store.sending)
            .onAppear { if store.draftAccount == nil { store.draftAccount = store.selectedAccount ?? store.accounts.first?.id } }
    }
}
