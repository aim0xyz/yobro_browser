import SwiftUI
import AppKit

/// YoBro Mail: mailboxes, the message list and the reader on one page card.
struct MailWorkspace: View {
    @ObservedObject var store: MailStore
    let close: () -> Void
    var openURL: (URL) -> Void = { NSWorkspace.shared.open($0) }
    var askAssistant: ((MailItem, MailBody, String) -> Void)? = nil
    @State private var showAccount = false
    @State private var editing: MailAccount?
    @State private var remove: MailAccount?
    @State private var bodyError: String?
    @State private var plainText = false
    @State private var selectionMode = false
    @State private var markedIDs: Set<String> = []
    @State private var pendingDelete: [MailItem] = []
    @State private var mailboxesExpanded = false

    var body: some View {
        GeometryReader { geometry in
            let compact = geometry.size.width < 880
            let singleColumn = geometry.size.width < 650
            VStack(spacing: 0) {
                header(compact: compact, singleColumn: singleColumn)
                Divider().opacity(0.5)
                ForEach(store.accounts.filter { store.lockedAccounts.contains($0.id) }) { account in
                    HStack(spacing: 8) {
                        Image(systemName: "lock.fill").foregroundStyle(.secondary)
                        Text(L("\(account.title): Mail-Zugriff freigeben", "\(account.title): allow mail access")).lineLimit(1)
                        Spacer()
                        Button(L("Mail entsperren", "Unlock mail")) { Task { await store.unlockMail(account.id) } }
                            .buttonStyle(YOBROPopupButtonStyle(prominent: true))
                            .disabled(store.refreshing || store.unlockingAccount != nil)
                    }
                    .font(.system(size: 12)).padding(.horizontal, 16).padding(.vertical, 8)
                    .background(YOBROTheme.field)
                }
                if store.accounts.isEmpty {
                    ScrollView { welcome.frame(minHeight: max(480, geometry.size.height - 60)) }
                } else {
                    HStack(spacing: 0) {
                        if !compact || mailboxesExpanded {
                            mailboxes.frame(width: 204)
                            Divider().opacity(0.5)
                        }
                        if singleColumn {
                            if store.selected != nil { reader.frame(maxWidth: .infinity, maxHeight: .infinity) }
                            else { messageList.frame(maxWidth: .infinity) }
                        } else {
                            messageList.frame(minWidth: 240, idealWidth: 310, maxWidth: 340)
                            Divider().opacity(0.5)
                            reader.frame(maxWidth: .infinity, maxHeight: .infinity)
                        }
                    }
                }
            }
            .onChange(of: compact) { _, compact in if compact { mailboxesExpanded = false } }
        }
        .background(paper).foregroundStyle(ink)
        .sheet(isPresented: $showAccount) { MailAccountEditor(store: store, existing: editing) }
        .sheet(isPresented: $store.composerPresented) { MailComposer(store: store) }
        .overlay {
            if let account = remove {
                YOBRODialogOverlay(icon: "person.crop.circle.badge.minus", title: L("Postfach entfernen?", "Remove mailbox?"),
                                   message: L("„\(account.title)“ und sein Passwort werden aus YoBro entfernt. Nachrichten beim Anbieter bleiben erhalten.",
                                              "“\(account.title)” and its password are removed from YoBro. Messages stay with your provider."),
                                   confirmTitle: L("Entfernen", "Remove"), cancelTitle: L("Abbrechen", "Cancel"), destructive: true, confirm: {
                    do { try store.disconnect(account) } catch { store.notice = error.localizedDescription }
                    remove = nil
                }, cancel: { remove = nil })
            } else if !pendingDelete.isEmpty {
                YOBRODialogOverlay(icon: "trash.fill", title: deleteTitle(for: pendingDelete),
                                   message: L("Diese Aktion wird direkt mit deinem Mailserver synchronisiert.", "This action is synchronized directly with your mail server."),
                                   confirmTitle: deleteTitle(for: pendingDelete), cancelTitle: L("Abbrechen", "Cancel"), destructive: true,
                                   confirm: { let messages = pendingDelete; pendingDelete = []; perform { try await store.delete(messages) } },
                                   cancel: { pendingDelete = [] })
            } else if let notice = store.notice {
                YOBRODialogOverlay(icon: "envelope.fill", title: "YoBro Mail", message: notice, confirmTitle: "OK", confirm: { store.notice = nil })
            }
        }
        .onChange(of: store.selectedAccount) { _, _ in endSelection() }
        .onChange(of: store.selectedFolder) { _, _ in endSelection() }
        .appearanceTask {
            if !store.accounts.isEmpty { await store.refresh() }
        }
    }

    // MARK: Header

    private var scopeTitle: String {
        guard let id = store.selectedAccount, let account = store.accounts.first(where: { $0.id == id }) else {
            return L("Alle Eingänge", "All inboxes")
        }
        let folder = store.folders[id]?.first { $0.path == store.selectedFolder }?.displayTitle ?? L("Posteingang", "Inbox")
        return "\(account.title) · \(folder)"
    }

    private func header(compact: Bool, singleColumn: Bool) -> some View {
        HStack(spacing: 4) {
            if singleColumn, store.selected != nil {
                headerButton("chevron.left", L("Zurück zu den Nachrichten", "Back to messages")) { store.selectedID = nil }
            }
            if compact, !store.accounts.isEmpty {
                headerButton("sidebar.left", mailboxesExpanded ? L("Postfächer ausblenden", "Hide mailboxes") : L("Postfächer einblenden", "Show mailboxes")) {
                    mailboxesExpanded.toggle()
                }
            }
            VStack(alignment: .leading, spacing: 1) {
                Text(L("Mail", "Mail")).font(.system(size: 13, weight: .semibold))
                Text(store.accounts.isEmpty ? L("Kein Postfach verbunden", "No mailbox connected") : scopeTitle)
                    .font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
            }
            .padding(.leading, 4)
            Spacer()
            if store.refreshing { ProgressView().controlSize(.small).padding(.horizontal, 6) }
            if !store.accounts.isEmpty {
                headerButton("arrow.clockwise", L("Alle Postfächer aktualisieren", "Refresh all mailboxes")) {
                    MailSecrets.session.retryFailures(); Task { await store.refresh() }
                }
                .disabled(store.refreshing)
                headerButton("square.and.pencil", L("Neue E-Mail · ⌘N", "New email · ⌘N")) { store.compose() }
                    .keyboardShortcut("n", modifiers: .command)
            }
            Menu {
                Button(store.notificationsEnabled ? L("Mitteilungen ausschalten", "Turn off notifications") : L("Mitteilungen einschalten", "Turn on notifications")) {
                    Task { await store.setNotifications(!store.notificationsEnabled) }
                }
                Toggle(L("Bilder von anderen Servern immer laden", "Always load images from other servers"),
                       isOn: Binding(get: { store.remoteImagesEnabled }, set: { store.setRemoteImages($0) }))
                if !store.trustedImageSenders.isEmpty {
                    Button(L("Freigegebene Absender vergessen (\(store.trustedImageSenders.count))", "Forget trusted senders (\(store.trustedImageSenders.count))")) {
                        store.forgetTrustedImageSenders()
                    }
                }
                Divider()
                Button(L("Postfach verbinden …", "Connect mailbox …")) { editing = nil; showAccount = true }
            } label: {
                Image(systemName: "ellipsis").font(.system(size: 13)).frame(width: 30, height: 30)
            }
            .menuStyle(.button).buttonStyle(YOBROButtonStyle(minimumSize: 30)).menuIndicator(.hidden).fixedSize()
            .foregroundStyle(ink.opacity(0.7))
            .yobroHelp(L("Mail-Einstellungen", "Mail settings"))
            headerButton("xmark", L("Mail schließen · ⇧⌘M", "Close mail · ⇧⌘M"), action: close)
        }
        .padding(.leading, 12).padding(.trailing, 8).frame(height: 52)
    }

    private func headerButton(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 13)).frame(width: 30, height: 30)
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 30))
        .foregroundStyle(ink.opacity(0.7))
        .yobroHelp(help).accessibilityLabel(help)
    }

    // MARK: Welcome

    private var welcome: some View {
        VStack(spacing: 18) {
            Spacer()
            Image(systemName: "tray.2").font(.system(size: 34, weight: .light)).foregroundStyle(ink.opacity(0.7))
                .frame(width: 76, height: 76).background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            Text(L("Alle Postfächer an einem Ort.", "All your mailboxes in one place."))
                .font(.system(size: 24, weight: .semibold)).multilineTextAlignment(.center)
            Text(L("Verbinde deine Konten, lies und beantworte E-Mails direkt in YoBro.",
                   "Connect your accounts, then read and answer email right in YoBro."))
                .font(.system(size: 13)).foregroundStyle(.secondary).multilineTextAlignment(.center)
            Button { editing = nil; showAccount = true } label: {
                Label(L("Postfach verbinden", "Connect a mailbox"), systemImage: "plus").padding(.horizontal, 6)
            }
            .buttonStyle(YOBROPopupButtonStyle(prominent: true))
            Text("Gmail · iCloud Mail · Spacemail · IMAP / SMTP").font(.system(size: 11)).foregroundStyle(.secondary).padding(.top, 6)
            Spacer()
            Label(L("Direkt zum Anbieter verbunden · Passwörter im Schlüsselbund", "Connected straight to your provider · passwords in the Keychain"), systemImage: "lock.shield")
                .font(.system(size: 11)).foregroundStyle(.secondary).padding(.bottom, 22)
        }
        .padding(.horizontal, 24).frame(maxWidth: .infinity)
    }

    // MARK: Mailboxes

    private var mailboxes: some View {
        VStack(alignment: .leading, spacing: 8) {
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    mailboxRow(L("Alle Eingänge", "All inboxes"), symbol: "tray.2", selected: store.selectedAccount == nil, unread: store.hasUnread) {
                        store.selectFolder(account: nil)
                    }
                    Text(L("Postfächer", "Mailboxes")).font(.system(size: 11, weight: .medium)).foregroundStyle(.secondary)
                        .padding(.horizontal, 10).padding(.top, 12).padding(.bottom, 2)
                    ForEach(store.accounts) { account in
                        mailboxRow(account.title, symbol: "envelope", selected: store.selectedAccount == account.id && store.selectedFolder.uppercased() == "INBOX",
                                   unread: store.hasUnread(account.id), warning: store.errors[account.id] != nil) {
                            store.selectFolder(account: account.id)
                        }
                        .contextMenu {
                            Button(L("Konto bearbeiten …", "Edit account …")) { editing = account; showAccount = true }
                            Button(L("Konto entfernen …", "Remove account …"), role: .destructive) { remove = account }
                        }
                        if store.selectedAccount == account.id {
                            ForEach((store.folders[account.id] ?? []).filter { $0.path.uppercased() != "INBOX" }) { folder in
                                mailboxRow(folder.displayTitle, symbol: folder.symbol, selected: store.selectedFolder == folder.path,
                                           count: folder.unread, indent: true) {
                                    store.selectFolder(account: account.id, folder: folder.path)
                                }
                                .disabled(!folder.selectable)
                            }
                        }
                    }
                }
                .padding(8)
            }
            VStack(alignment: .leading, spacing: 6) {
                Toggle(L("Nur ungelesene", "Unread only"), isOn: $store.unreadOnly).toggleStyle(.checkbox)
                Group {
                    if let date = store.lastRefresh {
                        Text(L("Aktualisiert um ", "Updated at ")) + Text(date, style: .time)
                    } else {
                        Text(L("Aktualisiert alle \(MailStore.pollingMinutes) Minuten", "Updates every \(MailStore.pollingMinutes) minutes"))
                    }
                }
                .foregroundStyle(.secondary)
            }
            .font(.system(size: 11)).padding(.horizontal, 16).padding(.bottom, 14)
        }
        .background(ink.opacity(0.025))
    }

    private func mailboxRow(_ title: String, symbol: String, selected: Bool, unread: Bool = false, count: Int = 0, warning: Bool = false,
                            indent: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: symbol).font(.system(size: 12)).frame(width: 16).foregroundStyle(ink.opacity(0.65))
                Text(title).lineLimit(1)
                Spacer(minLength: 0)
                if warning { Image(systemName: "exclamationmark.circle").foregroundStyle(.orange) }
                if count > 0 { Text("\(count)").font(.system(size: 11)).foregroundStyle(.secondary) }
                else if unread { MailUnreadDot() }
            }
            .font(.system(size: 12, weight: selected ? .semibold : .regular))
            .padding(.leading, indent ? 22 : 10).padding(.trailing, 10).frame(height: 30)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(selected ? ink.opacity(0.07) : .clear, in: RoundedRectangle(cornerRadius: 8))
            .contentShape(Rectangle())
        }
        .buttonStyle(YOBROButtonStyle())
        .yobroHelp(title)
    }

    // MARK: Message list

    private var showsAccountNames: Bool { store.selectedAccount == nil && store.accounts.count > 1 }

    private var messageList: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                HStack(spacing: 6) {
                    Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                    TextField(L("Suchen", "Search"), text: $store.search).textFieldStyle(.plain)
                }
                .padding(.horizontal, 9).frame(height: 30)
                .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 8))
                Button(selectionMode ? L("Fertig", "Done") : L("Auswählen", "Select")) {
                    if selectionMode { endSelection() } else { selectionMode = true }
                }
                .buttonStyle(.plain).foregroundStyle(ink.opacity(0.7))
            }
            .font(.system(size: 12)).padding(.horizontal, 12).padding(.top, 12).padding(.bottom, 8)
            if selectionMode {
                HStack {
                    Button(markedIDs.count == store.filtered.count && !store.filtered.isEmpty ? L("Auswahl aufheben", "Deselect all") : L("Alle", "All")) {
                        if markedIDs.count == store.filtered.count { markedIDs.removeAll() } else { markedIDs = Set(store.filtered.map(\.id)) }
                    }
                    Spacer()
                    Text(L("\(markedIDs.count) ausgewählt", "\(markedIDs.count) selected")).foregroundStyle(.secondary)
                }
                .buttonStyle(.plain).font(.system(size: 11)).padding(.horizontal, 14).padding(.bottom, 6)
            }
            ForEach(store.accounts.filter { store.errors[$0.id] != nil && (store.selectedAccount == nil || store.selectedAccount == $0.id) }) { account in
                VStack(alignment: .leading, spacing: 5) {
                    Text(account.address).fontWeight(.medium)
                    Text(store.errors[account.id] ?? "").fixedSize(horizontal: false, vertical: true)
                    Button(L("Kontodaten prüfen", "Check account")) { editing = account; showAccount = true }
                        .buttonStyle(.plain).underline()
                }
                .font(.system(size: 11)).foregroundStyle(.orange)
                .padding(10).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.orange.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
                .padding(.horizontal, 10).padding(.bottom, 6)
            }
            ScrollView {
                LazyVStack(spacing: 2) {
                    if store.filtered.isEmpty {
                        Text(store.refreshing || store.folderLoading ? L("Nachrichten werden geladen …", "Loading messages …") : L("Keine Nachrichten in dieser Ansicht.", "No messages here."))
                            .font(.system(size: 12)).foregroundStyle(.secondary).padding(24)
                    }
                    ForEach(store.filtered) { item in messageRow(item) }
                    if let account = store.selectedAccount, (store.folderTotals["\(account):\(store.selectedFolder)"] ?? 0) > store.filtered.count, store.search.isEmpty {
                        Button(L("Ältere Nachrichten laden", "Load older messages")) { Task { await store.loadMore() } }
                            .buttonStyle(YOBROPopupButtonStyle()).disabled(store.folderLoading).padding(10)
                    }
                }
                .padding(.horizontal, 8).padding(.bottom, 8)
            }
            if store.folderLoading { ProgressView().controlSize(.small).padding(6) }
            if selectionMode {
                Divider().opacity(0.5)
                HStack {
                    if let account = store.selectedAccount {
                        Menu {
                            ForEach((store.folders[account] ?? []).filter { $0.selectable && $0.path != store.selectedFolder }) { folder in
                                Button(folder.displayTitle) {
                                    let messages = markedItems
                                    perform { try await store.move(messages, to: folder.path) }
                                }
                            }
                        } label: { Label(L("Verschieben", "Move"), systemImage: "folder") }
                            .fixedSize()
                            .disabled(markedIDs.isEmpty || store.organizing)
                    }
                    Spacer()
                    if store.organizing { ProgressView().controlSize(.small) }
                    Button(role: .destructive) { pendingDelete = markedItems } label: { Label(deleteTitle(for: markedItems), systemImage: "trash") }
                        .disabled(markedIDs.isEmpty || store.organizing)
                }
                .buttonStyle(.bordered).font(.system(size: 11)).padding(10)
            }
        }
    }

    private func messageRow(_ item: MailItem) -> some View {
        let marked = markedIDs.contains(item.id)
        return Button {
            if selectionMode {
                if marked { markedIDs.remove(item.id) } else { markedIDs.insert(item.id) }
            } else { store.selectedID = item.id }
        } label: {
            HStack(alignment: .top, spacing: 8) {
                if selectionMode {
                    Image(systemName: marked ? "checkmark.circle.fill" : "circle").foregroundStyle(marked ? ink : ink.opacity(0.35))
                } else {
                    MailUnreadDot().opacity(item.unread ? 1 : 0).padding(.top, 5)
                }
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(item.senderName).font(.system(size: 12, weight: item.unread ? .semibold : .medium)).lineLimit(1)
                        Spacer(minLength: 6)
                        Text(Self.shortDate(item.date)).font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Text(item.subject.isEmpty ? L("(Ohne Betreff)", "(No subject)") : item.subject)
                        .font(.system(size: 12)).foregroundStyle(ink.opacity(item.unread ? 1 : 0.75)).lineLimit(2)
                    if showsAccountNames {
                        Text(store.accounts.first { $0.id == item.accountID }?.title ?? "").font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
            }
            .padding(.horizontal, 10).padding(.vertical, 9)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(marked || store.selectedID == item.id ? ink.opacity(0.07) : .clear, in: RoundedRectangle(cornerRadius: 8))
            .contentShape(Rectangle())
        }
        .buttonStyle(YOBROButtonStyle())
        .contextMenu {
            Button(L("Antworten", "Reply")) { store.reply(item) }
            if !item.unread { Button(L("Als ungelesen markieren", "Mark as unread")) { Task { await store.markUnread(item) } } }
            Button(deleteTitle(for: [item]), role: .destructive) { pendingDelete = [item] }
        }
    }

    /// Time for today's messages, the date otherwise.
    static func shortDate(_ date: Date) -> String {
        Calendar.current.isDateInToday(date) ? date.formatted(date: .omitted, time: .shortened) : date.formatted(.dateTime.day().month(.abbreviated))
    }

    private var markedItems: [MailItem] { store.items.filter { markedIDs.contains($0.id) } }
    private func isTrash(_ item: MailItem) -> Bool {
        store.folders[item.accountID]?.first(where: { $0.path == item.folder })?.isTrash == true
    }
    private func deleteTitle(for messages: [MailItem]) -> String {
        messages.contains(where: isTrash) ? L("Endgültig löschen", "Delete permanently") : L("In den Papierkorb", "Move to Trash")
    }
    private func endSelection() { selectionMode = false; markedIDs.removeAll() }
    private func perform(_ operation: @escaping () async throws -> Void) {
        Task {
            do { try await operation(); endSelection() }
            catch { store.notice = error.localizedDescription }
        }
    }

    // MARK: Reader

    private var reader: some View {
        Group {
            if let item = store.selected {
                VStack(alignment: .leading, spacing: 0) {
                    readerHeader(item)
                    Divider().opacity(0.5)
                    if let body = store.bodies[item.id] {
                        readerContent(item, body)
                    } else if let error = bodyError {
                        VStack(alignment: .leading, spacing: 10) {
                            Text(error).font(.system(size: 12)).foregroundStyle(.orange)
                            Button(L("Erneut laden", "Try again")) {
                                Task { bodyError = nil; do { try await store.load(item) } catch { bodyError = error.localizedDescription } }
                            }
                            .buttonStyle(YOBROPopupButtonStyle())
                        }
                        .padding(20)
                        Spacer()
                    } else {
                        ProgressView(L("Nachricht wird geladen …", "Loading message …")).padding(20).frame(maxWidth: .infinity)
                        Spacer()
                    }
                }
                .appearanceTask(id: item.id) {
                    bodyError = nil; plainText = false
                    do { try await store.load(item) } catch { if store.selectedID == item.id { bodyError = error.localizedDescription } }
                }
            } else {
                VStack(spacing: 10) {
                    Image(systemName: "envelope.open").font(.system(size: 30, weight: .light)).foregroundStyle(.secondary)
                    Text(L("Keine Nachricht ausgewählt", "No message selected")).font(.system(size: 15, weight: .semibold))
                    Text(L("Wähle eine E-Mail aus der Liste.", "Choose an email from the list.")).font(.system(size: 12)).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }

    private func readerHeader(_ item: MailItem) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(item.subject.isEmpty ? L("(Ohne Betreff)", "(No subject)") : item.subject)
                .font(.system(size: 19, weight: .semibold)).lineLimit(3).textSelection(.enabled)
            HStack(alignment: .top, spacing: 10) {
                VStack(alignment: .leading, spacing: 3) {
                    (Text(item.senderName).fontWeight(.semibold) + Text(item.senderName == item.senderAddress ? "" : "  " + item.senderAddress).foregroundColor(.secondary))
                        .font(.system(size: 12)).lineLimit(1).textSelection(.enabled)
                    Text(L("An: ", "To: ") + item.recipient).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(2).textSelection(.enabled)
                    Text(item.date, format: .dateTime.day().month().year().hour().minute()).font(.system(size: 11)).foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                HStack(spacing: 2) {
                    if let askAssistant, let body = store.bodies[item.id] {
                        Menu {
                            ForEach([L("Fasse diese E-Mail zusammen.", "Summarize this email."),
                                     L("Entwirf eine Antwort auf diese E-Mail.", "Draft a reply to this email."),
                                     L("Öffne die Links aus dieser E-Mail.", "Open the links from this email.")], id: \.self) { prompt in
                                Button(prompt) { askAssistant(item, body, prompt) }
                            }
                        } label: {
                            Image(systemName: "sparkles").font(.system(size: 13)).frame(width: 30, height: 30)
                        }
                        .menuStyle(.button).buttonStyle(YOBROButtonStyle(minimumSize: 30)).menuIndicator(.hidden).fixedSize()
                        .foregroundStyle(ink.opacity(0.7))
                        .yobroHelp(L("Mit dem Assistenten bearbeiten", "Work on it with the assistant"))
                    }
                    headerButton("envelope.badge", L("Als ungelesen markieren", "Mark as unread")) { Task { await store.markUnread(item) } }
                    headerButton("trash", deleteTitle(for: [item])) { pendingDelete = [item] }
                    Button { store.reply(item) } label: { Label(L("Antworten", "Reply"), systemImage: "arrowshape.turn.up.left") }
                        .buttonStyle(YOBROPopupButtonStyle()).padding(.leading, 4)
                }
            }
        }
        .padding(.horizontal, 20).padding(.top, 16).padding(.bottom, 12)
    }

    @ViewBuilder
    private func readerContent(_ item: MailItem, _ body: MailBody) -> some View {
        let showsHTML = !body.html.isEmpty && !plainText
        let imagesAllowed = store.remoteImagesAllowed(for: item)
        if !body.html.isEmpty || !body.attachments.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                if showsHTML && body.hasRemoteImages && !imagesAllowed {
                    HStack(spacing: 8) {
                        Image(systemName: "photo").foregroundStyle(.secondary)
                        Text(L("Bilder von anderen Servern sind blockiert.", "Images from other servers are blocked.")).lineLimit(1)
                        Spacer()
                        Button(L("Laden", "Load")) { store.allowRemoteImages(for: item, always: false) }
                        Button(L("Immer von diesem Absender", "Always from this sender")) { store.allowRemoteImages(for: item, always: true) }
                    }
                    .buttonStyle(.plain).foregroundStyle(ink.opacity(0.8))
                    .yobroHelp(L("Externe Bilder können dem Absender verraten, dass du die Nachricht geöffnet hast.", "Remote images can tell the sender that you opened the message."))
                    .padding(.horizontal, 10).frame(height: 30)
                    .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 8))
                }
                HStack(spacing: 8) {
                    if !body.attachments.isEmpty {
                        ScrollView(.horizontal) {
                            HStack(spacing: 6) {
                                ForEach(body.attachments) { attachment in
                                    Button { store.download(attachment, from: item) } label: {
                                        HStack(spacing: 6) {
                                            Image(systemName: "paperclip")
                                            Text(attachment.name).lineLimit(1)
                                            Text(ByteCountFormatter.string(fromByteCount: Int64(attachment.size), countStyle: .file)).foregroundStyle(.secondary)
                                        }
                                        .font(.system(size: 11)).padding(.horizontal, 9).frame(height: 26)
                                        .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 7))
                                    }
                                    .buttonStyle(YOBROButtonStyle())
                                    .disabled(store.attachmentDownloads.contains(item.id + ":" + attachment.id))
                                    .yobroHelp(L("Anhang speichern: ", "Save attachment: ") + attachment.name)
                                }
                            }
                        }
                        .scrollIndicators(.hidden)
                    }
                    Spacer(minLength: 0)
                    if !body.html.isEmpty {
                        Toggle(L("Nur Text", "Plain text"), isOn: $plainText).toggleStyle(.checkbox).font(.system(size: 11)).fixedSize()
                    }
                }
            }
            .padding(.horizontal, 20).padding(.vertical, 10)
        }
        if showsHTML {
            MailHTMLView(key: item.id, html: body.html, remoteImages: imagesAllowed, openURL: openURL)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                Text(body.text.isEmpty
                     ? (body.attachments.isEmpty ? L("Diese Nachricht enthält keinen darstellbaren Text.", "This message has no text to show.") : L("Der Inhalt befindet sich in den Anhängen.", "The content is in the attachments."))
                     : body.text)
                    .font(.system(size: 13)).lineSpacing(4).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(20)
            }
            .frame(maxHeight: .infinity)
        }
    }
}

// MARK: - Account editor

struct MailAccountEditor: View {
    @ObservedObject var store: MailStore
    let existing: MailAccount?
    @Environment(\.dismiss) private var dismiss
    @State private var account = MailAccount()
    @State private var password = ""
    @State private var provider = "Automatisch"
    @State private var explanation = L("YoBro erkennt die Server anhand deiner E-Mail-Adresse.", "YoBro finds the servers from your email address.")
    @State private var source = ""
    @State private var detecting = false
    @State private var oauthOnly = false
    @State private var expanded = false
    @State private var error: String?
    @State private var lookup: Task<Void, Never>?
    @State private var revision = UUID()
    @FocusState private var focused: String?

    private var canConnect: Bool {
        !store.connecting && !detecting && !oauthOnly && (!password.isEmpty || existing != nil)
            && !account.address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !account.imapHost.isEmpty && !account.smtpHost.isEmpty
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "envelope.badge.shield.half.filled")
                    .font(.system(size: 19, weight: .light)).foregroundStyle(ink.opacity(0.75))
                    .frame(width: 42, height: 42).background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
                VStack(alignment: .leading, spacing: 3) {
                    Text(existing == nil ? L("Postfach verbinden", "Connect a mailbox") : L("Postfach bearbeiten", "Edit mailbox"))
                        .font(.system(size: 17, weight: .semibold))
                    Text(L("Alle Nachrichten an einem Ort.", "All your messages in one place.")).font(.system(size: 12)).foregroundStyle(.secondary)
                }
                Spacer()
                Button { dismiss() } label: {
                    Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
                        .frame(width: 26, height: 26).background(ink.opacity(0.055), in: Circle())
                }
                .buttonStyle(YOBROButtonStyle()).yobroHelp(L("Schließen", "Close")).keyboardShortcut(.cancelAction).disabled(store.connecting)
            }
            .padding(24)
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    field(L("E-Mail-Adresse", "Email address")) {
                        TextField(L("du@beispiel.de", "you@example.com"), text: $account.address).focused($focused, equals: "email")
                            .onSubmit { schedule(immediate: true) }
                    }
                    HStack {
                        Text(L("Anbieter", "Provider")).foregroundStyle(.secondary)
                        Spacer()
                        Picker(L("Anbieter", "Provider"), selection: $provider) {
                            ForEach(["Automatisch", "Gmail", "iCloud Mail", "Spacemail", "Outlook / Microsoft 365", "Manuell"], id: \.self) { Text(Self.providerTitle($0)).tag($0) }
                        }
                        .labelsHidden().fixedSize().accessibilityLabel(L("Mail-Anbieter", "Mail provider"))
                    }
                    .font(.system(size: 12))
                    field(L("Passwort", "Password")) {
                        SecureField(existing == nil ? L("App-Passwort oder Postfachpasswort", "App password or mailbox password")
                                                    : L("Leer lassen, um das gespeicherte zu behalten", "Leave empty to keep the saved one"), text: $password)
                            .disabled(oauthOnly)
                    }
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: oauthOnly ? "info.circle" : "key.horizontal").foregroundStyle(.secondary)
                        Text(explanation).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    .font(.system(size: 11)).lineSpacing(3)
                    if let help = MailPasswordProvider.detect(provider: provider, account: account) {
                        MailPasswordHelp(provider: help)
                    }
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Button { expanded.toggle() } label: {
                                HStack(spacing: 8) {
                                    Image(systemName: "chevron.right").rotationEffect(.degrees(expanded ? 90 : 0)).font(.system(size: 9, weight: .semibold))
                                    Text(L("Server & Kontodetails", "Server & account details")).font(.system(size: 12, weight: .medium))
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(YOBROButtonStyle()).accessibilityValue(expanded ? L("Ausgeklappt", "Expanded") : L("Eingeklappt", "Collapsed"))
                            Spacer()
                            if detecting { ProgressView().controlSize(.mini) }
                            else if !account.imapHost.isEmpty { Label("TLS", systemImage: "lock.fill").font(.system(size: 10)).foregroundStyle(.secondary) }
                        }
                        if !expanded {
                            Text(detecting ? L("Server werden gesucht …", "Looking for servers …")
                                 : account.imapHost.isEmpty ? L("Werden automatisch eingerichtet", "Set up automatically")
                                 : "IMAP  \(account.imapHost):\(account.imapPort)\nSMTP  \(account.smtpHost):\(account.smtpPort)")
                                .font(.system(size: 11)).foregroundStyle(.secondary).lineSpacing(4).textSelection(.enabled)
                        } else {
                            field(L("Kontoname", "Account name")) { TextField(L("Mein Postfach", "My mailbox"), text: $account.label) }
                            field(L("Benutzername", "Username")) { TextField(account.address, text: $account.username) }
                            HStack(alignment: .top, spacing: 10) {
                                field(L("IMAP-Server", "IMAP server")) { TextField(L("imap.beispiel.de", "imap.example.com"), text: manual(\.imapHost)) }
                                field("Port") { TextField("993", value: manual(\.imapPort), format: .number.grouping(.never)) }.frame(width: 70)
                            }
                            HStack(alignment: .top, spacing: 10) {
                                field(L("SMTP-Server", "SMTP server")) { TextField(L("smtp.beispiel.de", "smtp.example.com"), text: manual(\.smtpHost)) }
                                field("Port") { TextField("465", value: manual(\.smtpPort), format: .number.grouping(.never)) }.frame(width: 70)
                            }
                            field(L("SMTP-Benutzername · optional", "SMTP username · optional")) {
                                TextField(L("Wie Benutzername", "Same as username"), text: Binding(get: { account.smtpUsername ?? "" }, set: { account.smtpUsername = $0 }))
                            }
                            Picker(L("SMTP-Sicherheit", "SMTP security"), selection: manual(\.smtpSecurity)) {
                                Text(L("TLS direkt", "Direct TLS")).tag("tls"); Text("STARTTLS").tag("starttls")
                            }
                            .font(.system(size: 11))
                            if !source.isEmpty { Text(source).font(.system(size: 10)).foregroundStyle(.secondary) }
                            Button(L("Server erneut erkennen", "Detect servers again")) { provider = "Automatisch"; schedule(immediate: true) }
                                .buttonStyle(YOBROButtonStyle()).font(.system(size: 11)).disabled(detecting || account.address.isEmpty)
                            Text(L("Für die Erkennung wird nur die Domain bei deinem Anbieter und gegebenenfalls der Thunderbird-Anbieterdatenbank abgefragt.",
                                   "Detection only sends the domain to your provider and, if needed, Thunderbird's provider database."))
                                .font(.system(size: 10)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(14).background(YOBROTheme.field.opacity(0.6), in: RoundedRectangle(cornerRadius: 12))
                    if let error {
                        Label(error, systemImage: "exclamationmark.circle").font(.system(size: 11)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.horizontal, 24).padding(.bottom, 20).disabled(store.connecting)
            }
            .frame(height: 400)
            Divider().opacity(0.5)
            HStack(spacing: 16) {
                Label(L("Sicher im macOS-Schlüsselbund", "Kept safe in the macOS Keychain"), systemImage: "lock.shield")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                Button(action: connect) {
                    HStack(spacing: 8) {
                        if store.connecting { ProgressView().controlSize(.small) }
                        Text(store.connecting ? L("Wird geprüft …", "Checking …") : existing == nil ? L("Verbinden", "Connect") : L("Sichern", "Save"))
                    }
                    .padding(.horizontal, 6)
                }
                .buttonStyle(YOBROPopupButtonStyle(prominent: true))
                .keyboardShortcut(.defaultAction)
                .disabled(!canConnect)
            }
            .padding(.horizontal, 24).padding(.vertical, 16)
        }
        .frame(width: 540).background(paper).foregroundStyle(ink).interactiveDismissDisabled(store.connecting)
        .onAppear {
            if let existing { account = existing; provider = "Manuell"; expanded = true }
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

    static func providerTitle(_ value: String) -> String {
        switch value {
        case "Automatisch": return L("Automatisch", "Automatic")
        case "Manuell": return L("Manuell", "Manual")
        default: return value
        }
    }

    private func field<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 11, weight: .medium)).foregroundStyle(.secondary)
            content().textFieldStyle(.plain).font(.system(size: 13)).padding(.horizontal, 11).frame(height: 34)
                .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 8))
                .accessibilityLabel(title)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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
            oauthOnly = false
            explanation = L("Verwende das Postfachpasswort oder ein App-Passwort deines Anbieters.", "Use the mailbox password or an app password from your provider.")
            source = L("Manuelle Konfiguration", "Manual setup"); return
        }
        if provider != "Automatisch" { apply(MailDiscovery.preset(provider, address: account.address, id: account.id)); return }
        account.imapHost = ""; account.smtpHost = ""; source = ""; oauthOnly = false
        explanation = L("YoBro erkennt die Server anhand deiner E-Mail-Adresse.", "YoBro finds the servers from your email address.")
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
                if submitted.username.trimmingCharacters(in: .whitespaces).isEmpty { submitted.username = submitted.address }
                if submitted.label.trimmingCharacters(in: .whitespaces).isEmpty { submitted.label = submitted.address }
                try await store.connect(submitted, password: password)
                password = ""; dismiss(); await store.refresh()
            } catch { self.error = error.localizedDescription }
        }
    }
}

// MARK: - Composer

struct MailComposer: View {
    @ObservedObject var store: MailStore
    @Environment(\.dismiss) private var dismiss
    @State private var error: String?
    @State private var showsCc = false
    @StateObject private var commands = RichTextCommands()

    private var canSend: Bool {
        !store.sending && !store.draftTo.trimmingCharacters(in: .whitespaces).isEmpty
            && !(store.draftText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && store.draftSubject.trimmingCharacters(in: .whitespaces).isEmpty)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(store.draftReplyID.isEmpty ? L("Neue E-Mail", "New email") : L("Antworten", "Reply")).font(.system(size: 17, weight: .semibold))
                Spacer()
                Button(L("Schließen", "Close")) { dismiss() }.buttonStyle(YOBROPopupButtonStyle()).keyboardShortcut(.cancelAction).disabled(store.sending)
            }
            HStack {
                Text(L("Von", "From")).font(.system(size: 12)).foregroundStyle(.secondary).frame(width: 44, alignment: .leading)
                Picker(L("Von", "From"), selection: $store.draftAccount) { ForEach(store.accounts) { Text($0.address).tag(Optional($0.id)) } }
                    .labelsHidden()
            }
            HStack(spacing: 8) {
                TextField(L("An", "To"), text: $store.draftTo).textFieldStyle(YOBROTextFieldStyle())
                if !showsCc { Button("Cc") { showsCc = true }.buttonStyle(YOBROPopupButtonStyle()) }
            }
            if showsCc { TextField("Cc", text: $store.draftCc).textFieldStyle(YOBROTextFieldStyle()) }
            TextField(L("Betreff", "Subject"), text: $store.draftSubject).textFieldStyle(YOBROTextFieldStyle())
            VStack(spacing: 0) {
                RichTextToolbar(commands: commands).padding(.horizontal, 10).frame(height: 38)
                Divider().opacity(0.4)
                RichTextEditor(rtf: store.draftRTF, fallbackText: store.draftText, commands: commands, adaptsEmailTextColor: true) { rtf, text in
                    store.draftRTF = rtf
                    store.draftText = text
                }
            }
            .background(YOBROTheme.field.opacity(0.6), in: RoundedRectangle(cornerRadius: 10))
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .frame(height: 320)
            if let error { Text(error).font(.system(size: 12)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true) }
            HStack {
                Text(L("Der Entwurf bleibt bis zum Beenden von YoBro erhalten. Eine Kopie landet im Gesendet-Ordner.",
                       "The draft stays until you quit YoBro. A copy goes to your Sent folder."))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                Spacer()
                if store.sending { ProgressView().controlSize(.small) }
                Button {
                    Task { error = nil; do { try await store.send(); dismiss() } catch { self.error = error.localizedDescription } }
                } label: { Label(L("Senden", "Send"), systemImage: "paperplane") }
                    .buttonStyle(YOBROPopupButtonStyle(prominent: true))
                    .keyboardShortcut(.return, modifiers: .command)
                    .disabled(!canSend)
                    .yobroHelp(L("Senden · ⌘↩", "Send · ⌘↩"))
            }
        }
        .padding(24).frame(width: 640)
        .background(YOBROArrowCursorRegion().ignoresSafeArea())
        .background(paper).foregroundStyle(ink).disabled(store.sending).interactiveDismissDisabled(store.sending)
        .onAppear {
            if store.draftAccount == nil { store.draftAccount = store.selectedAccount ?? store.accounts.first?.id }
            showsCc = !store.draftCc.isEmpty
        }
    }
}
