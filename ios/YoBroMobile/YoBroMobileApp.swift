import SwiftUI
import WebKit

@main struct YoBroMobileApp: App {
    @StateObject private var account = MobileAccount()
    @Environment(\.scenePhase) private var scenePhase
    var body: some Scene {
        WindowGroup {
            Group {
                MobileShell(userID: account.browserProfileID).id(account.browserProfileID)
            }
            .environmentObject(account).tint(YOBROTheme.accent).foregroundStyle(YOBROTheme.text)
            .onOpenURL { url in Task { await account.handle(url) } }
            .task { await account.refresh() }
            .onChange(of: scenePhase) { _, value in if value == .active { Task { await account.refresh() } } }
        }
    }
}

struct MobileLogin: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var account: MobileAccount
    @State private var email = ""
    @State private var code = ""
    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    MobileBrandMark(size: 72).padding(.top, 60)
                    Text("Dein Browser.\nÜberall dabei.").font(.largeTitle.bold())
                    Text(account.otpRequested ? "Enter the code we sent to \(account.otpEmail)." : "Sign in or create your YoBro account with a one-time code.").foregroundStyle(.secondary)
                    VStack(spacing: 16) {
                        if account.otpRequested {
                            TextField("One-time code", text: $code).textContentType(.oneTimeCode).keyboardType(.numberPad)
                        } else {
                            TextField("Email", text: $email).textContentType(.username).keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
                        }
                    }.padding().background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 18))
                    if let message = account.message { Text(message).font(.callout) }
                    if let error = account.error { Text(error).foregroundStyle(.red).font(.callout) }
                    Button {
                        Task {
                            if account.otpRequested { await account.verifyOTP(code); code = "" }
                            else { await account.requestOTP(email: email) }
                        }
                    } label: {
                        HStack { Spacer(); if account.busy { ProgressView() } else { Text(account.otpRequested ? "Verify code" : "Email me a code").bold(); Image(systemName: "arrow.right") }; Spacer() }.padding(8)
                    }.buttonStyle(.borderedProminent).disabled(account.busy || (account.otpRequested ? code.isEmpty : email.isEmpty))
                    Button("Ohne Konto weiter") { dismiss() }.frame(maxWidth: .infinity).padding(.vertical, 4)
                    if account.otpRequested { Button("Use a different email") { account.cancelOTP(); code = "" }.font(.footnote) }
                    Text("Ein Konto ist optional. Deine lokalen Daten bleiben beim Anmelden erhalten und werden getrennt vom Konto gespeichert.").font(.footnote).foregroundStyle(.secondary)
                }.padding(28).frame(maxWidth: 520)
            }.mobileSurface().navigationTitle("YoBro").navigationBarTitleDisplayMode(.inline)
        }
    }
}

struct MobileShell: View {
    @EnvironmentObject private var account: MobileAccount
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var browser: MobileBrowser
    @State private var address = ""
    @State private var panel: Panel?
    @State private var promptText = ""
    @FocusState private var addressFocused: Bool
    enum Panel: String, Identifiable { case tabs, notes, settings, downloads, bookmarks, history, login; var id: String { rawValue } }
    init(userID: UUID) { _browser = StateObject(wrappedValue: MobileBrowser(userID: userID)) }
    var body: some View {
        VStack(spacing: 0) {
            if !browser.ready && browser.selected?.url.isEmpty == false {
                ProgressView("Browser wird vorbereitet …").frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if browser.ready, let tab = browser.selected, !tab.url.isEmpty {
                MobileWebView(view: browser.webView(for: tab.id)).id(tab.id)
            } else {
                VStack(spacing: 20) {
                    Spacer()
                    MobileBrandMark(size: 72)
                    Text("Hey, wohin geht’s?").font(.largeTitle.bold())
                    Text("Dein Platz für neue Gedanken.").foregroundStyle(.secondary)
                    if !browser.ready || browser.protectionBusy {
                        ProgressView("Werbeschutz wird vorbereitet …").font(.caption).tint(YOBROTheme.accent)
                    }
                    Button("Suchen oder Website öffnen") { addressFocused = true }.buttonStyle(.bordered)
                    HStack {
                        Button("Notizen", systemImage: "note.text") { panel = .notes }
                        Button("Tabs", systemImage: "square.on.square") { panel = .tabs }
                    }.padding()
                    Button(account.isGuest ? "Mit deinem Konto anmelden" : "Mein Konto", systemImage: "person.crop.circle") {
                        panel = account.isGuest ? .login : .settings
                    }.buttonStyle(.borderedProminent)
                    Spacer()
                }.frame(maxWidth: .infinity)
            }
        }
        .background(YOBROTheme.page)
        .safeAreaInset(edge: .bottom, spacing: 0) { toolbar }
        .sheet(item: $panel) { item in
            if item == .login {
                MobileLogin().presentationDragIndicator(.visible)
            } else {
            NavigationStack {
                Group {
                    switch item {
                    case .login: EmptyView()
                    case .tabs: tabs
                    case .notes: MobileNotes(browser: browser)
                    case .settings: MobileSettings(browser: browser)
                    case .downloads: MobileDownloadList(downloads: browser.downloads)
                    case .bookmarks: library(history: false)
                    case .history: library(history: true)
                    }
                }.toolbar { ToolbarItem(placement: .confirmationAction) { Button("Fertig") { panel = nil } } }
            }.mobileSurface().presentationBackground(YOBROTheme.page).presentationDragIndicator(.visible)
            }
        }
        .sheet(item: $browser.extensionPage, onDismiss: { browser.extensionPage?.onClose() }) { page in
            NavigationStack {
                MobileWebView(view: page.view)
                    .navigationTitle(page.title).navigationBarTitleDisplayMode(.inline)
                    .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Fertig") { page.onClose(); browser.extensionPage = nil } } }
                    .onDisappear { page.onClose() }
            }.mobileSurface()
        }
        .alert(browser.dialog?.origin ?? "Webseite", isPresented: Binding(get: { browser.dialog != nil }, set: { if !$0 { browser.dismissDialog() } })) {
            if browser.dialog?.kind == .prompt { TextField("Eingabe", text: $promptText) }
            Button("OK") { let dialog = browser.dialog; browser.dialog = nil; dialog?.finish(promptText) }
            if browser.dialog?.kind != .alert { Button("Abbrechen", role: .cancel) { browser.dismissDialog() } }
        } message: { Text(browser.dialog?.message ?? "") }
        .onChange(of: browser.dialog?.id) { _, _ in promptText = browser.dialog?.defaultText ?? "" }
        .alert("YoBro", isPresented: Binding(get: { browser.error != nil }, set: { if !$0 { browser.error = nil } })) {
            Button("OK") { browser.error = nil }
        } message: { Text(browser.error ?? "") }
        .task {
            browser.handleAuthURL = { url in Task { await account.handle(url) } }
            address = browser.selected?.url ?? ""; await browser.prepare()
        }
        .onChange(of: browser.selected?.url) { _, url in if !addressFocused { address = url ?? "" } }
        .onChange(of: scenePhase) { _, phase in if phase != .active { browser.save() } }
        .onDisappear { browser.suspend() }
    }
    private var toolbar: some View {
        VStack(spacing: 12) {
            if browser.active?.isLoading == true { ProgressView(value: browser.active?.estimatedProgress ?? 0).tint(YOBROTheme.accent) }
            HStack {
                Image(systemName: browser.selected?.url.hasPrefix("https:") == true ? "lock.fill" : "magnifyingglass").foregroundStyle(.secondary)
                TextField("Suchen oder Adresse eingeben", text: $address)
                    .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.webSearch)
                    .submitLabel(.go).focused($addressFocused)
                    .onSubmit { browser.navigate(address); addressFocused = false }
                Button { browser.active?.reload() } label: { Image(systemName: "arrow.clockwise") }.accessibilityLabel("Neu laden")
            }.padding(12).background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 16))
            HStack {
                Button { browser.active?.goBack() } label: { Image(systemName: "chevron.left") }.disabled(browser.active?.canGoBack != true).accessibilityLabel("Zurück")
                Spacer()
                Button { browser.active?.goForward() } label: { Image(systemName: "chevron.right") }.disabled(browser.active?.canGoForward != true).accessibilityLabel("Vorwärts")
                Spacer()
                if let url = browser.selected.flatMap({ URL(string: $0.url) }), !url.absoluteString.isEmpty {
                    ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }.accessibilityLabel("Seite teilen")
                } else { Image(systemName: "square.and.arrow.up").foregroundStyle(.tertiary) }
                Spacer()
                Button { panel = .tabs } label: { Label("\(browser.state.tabs.count)", systemImage: "square.on.square") }.accessibilityLabel("\(browser.state.tabs.count) Tabs")
                Spacer()
                Menu {
                    Button(account.isGuest ? "Anmelden" : "Mein Konto", systemImage: "person.crop.circle") {
                        panel = account.isGuest ? .login : .settings
                    }
                    Divider()
                    Button("Neuer Tab", systemImage: "plus") { browser.add(); address = ""; addressFocused = true }
                    if !browser.availableSpaces.isEmpty {
                        Menu("Space auswählen", systemImage: "square.grid.2x2") {
                            ForEach(browser.availableSpaces, id: \.self) { space in
                                Button {
                                    browser.selectSpace(space)
                                    panel = .tabs
                                } label: {
                                    Label(space, systemImage: browser.currentSpace == space ? "checkmark.circle.fill" : "circle")
                                }
                            }
                        }
                    }
                    Button("Notizen", systemImage: "note.text") { panel = .notes }
                    Button("Auf Seite suchen", systemImage: "magnifyingglass") { browser.active?.findInteraction?.presentFindNavigator(showingReplace: false) }
                    Button("Geschlossenen Tab öffnen", systemImage: "arrow.uturn.backward") { browser.reopenClosedTab() }.disabled(browser.state.closedTabs?.isEmpty != false)
                    Button("Lesezeichen hinzufügen", systemImage: "bookmark") { browser.bookmarkCurrent() }
                    Button("Lesezeichen", systemImage: "book") { panel = .bookmarks }
                    Button("Verlauf", systemImage: "clock") { panel = .history }
                    Button("Downloads", systemImage: "arrow.down.circle") { panel = .downloads }
                    Button("Dunkle Darstellung für diese Website wechseln", systemImage: "moon") { Task { await browser.toggleDarkForCurrentSite() } }
                    if #available(iOS 18.6, *), browser.extensions?.isReady == true {
                        Button("uBlock Origin Lite", systemImage: "shield.lefthalf.filled") { browser.extensions?.performAction() }
                    }
                    Button("Einstellungen", systemImage: "gearshape") { panel = .settings }
                } label: { Image(systemName: "ellipsis.circle") }.accessibilityLabel("Browser-Menü")
            }.font(.title3).padding(.horizontal, 8).frame(minHeight: 32)
        }.padding(.horizontal, 16).padding(.vertical, 10).background(LinearGradient(colors: [YOBROTheme.chromeTop, YOBROTheme.chromeBottom], startPoint: .top, endPoint: .bottom))
    }
    private func library(history: Bool) -> some View {
        List {
            let entries = history ? browser.state.history ?? [] : browser.state.bookmarks ?? []
            if entries.isEmpty { ContentUnavailableView(history ? "Noch kein Verlauf" : "Noch keine Lesezeichen", systemImage: history ? "clock" : "bookmark") }
            ForEach(entries) { entry in
                Button { browser.navigate(entry.url); address = entry.url; panel = nil } label: {
                    VStack(alignment: .leading) { Text(entry.title).lineLimit(1); Text(entry.url).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
                }.listRowBackground(YOBROTheme.surface).swipeActions {
                    Button("Löschen", role: .destructive) {
                        if history { browser.state.history?.removeAll { $0.id == entry.id } }
                        else { browser.state.bookmarks?.removeAll { $0.id == entry.id } }; browser.save()
                    }
                }
            }
        }.mobileSurface().navigationTitle(history ? "Verlauf" : "Lesezeichen")
    }
    private var tabs: some View {
        List {
            Button("Neuer Tab", systemImage: "plus") { browser.add(); address = ""; panel = nil }
            if !browser.availableSpaces.isEmpty {
                Section("Space") {
                    Picker("Space", selection: Binding(get: { browser.currentSpace }, set: { if let space = $0 { browser.selectSpace(space) } })) {
                        ForEach(browser.availableSpaces, id: \.self) { Text($0).tag(Optional($0)) }
                    }
                }
            }
            let tabs = browser.tabsInCurrentSpace
            let folders = browser.foldersInCurrentSpace
            let folderIDs = Set(folders.map(\.id))
            ForEach(folders) { folder in
                Section {
                    if !folder.collapsed { tabRows(tabs.filter { $0.folderID == folder.id }) }
                } header: {
                    Button { browser.toggleFolder(folder.id) } label: {
                        HStack {
                            Image(systemName: folder.collapsed ? "chevron.right" : "chevron.down")
                            Image(systemName: "folder.fill")
                            Text(folder.name)
                            Spacer()
                            Text("\(tabs.filter { $0.folderID == folder.id }.count)").foregroundStyle(.secondary)
                        }
                    }.buttonStyle(.plain).accessibilityLabel("\(folder.name), \(folder.collapsed ? "eingeklappt" : "ausgeklappt")")
                }
            }
            let looseTabs = tabs.filter { $0.folderID == nil || !folderIDs.contains($0.folderID!) }
            if !looseTabs.isEmpty {
                Section(folders.isEmpty ? "Tabs" : "Ohne Ordner") { tabRows(looseTabs) }
            }
            if tabs.isEmpty {
                ContentUnavailableView("Keine Tabs in diesem Space", systemImage: "square.on.square")
            }
        }.mobileSurface().navigationTitle(browser.currentSpace.map { "Tabs · \($0)" } ?? "Deine Tabs")
    }
    @ViewBuilder private func tabRows(_ tabs: [MobileTab]) -> some View {
        ForEach(tabs) { tab in
                Button {
                    browser.select(tab.id); address = tab.url; panel = nil
                } label: {
                    HStack {
                        Image(systemName: tab.id == browser.state.selected ? "checkmark.circle.fill" : "globe")
                        VStack(alignment: .leading) { Text(tab.title).lineLimit(1); Text(tab.url.isEmpty ? "Startseite" : tab.url).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
                    }.padding(.vertical, 8)
                }.listRowBackground(YOBROTheme.surface).swipeActions { Button("Schließen", role: .destructive) { browser.close(tab.id) } }
        }
    }
}
struct MobileWebView: UIViewRepresentable {
    let view: WKWebView
    func makeUIView(context: Context) -> WKWebView { view }
    func updateUIView(_ uiView: WKWebView, context: Context) {}
}

struct MobileNotes: View {
    @ObservedObject var browser: MobileBrowser
    var body: some View {
        List {
            Button("Notiz zur aktuellen Seite", systemImage: "square.and.pencil") {
                browser.state.notes.insert(MobileNote(source: browser.selected?.url ?? ""), at: 0); browser.save()
            }
            ForEach($browser.state.notes) { $note in
                NavigationLink {
                    MobileNoteEditor(note: $note, browser: browser)
                } label: { MobileNoteRow(note: note) }.listRowBackground(YOBROTheme.surface)
            }.onDelete { browser.state.notes.remove(atOffsets: $0); browser.save() }
        }.mobileSurface().navigationTitle("Notizen").onChange(of: browser.state.notes.map(\.text)) { _, _ in browser.save() }
            .onChange(of: browser.state.notes.map(\.title)) { _, _ in browser.save() }
    }
}

struct MobileNoteRow: View {
    let note: MobileNote
    var body: some View {
        VStack(alignment: .leading) {
            Text(note.title)
            Text(note.text.isEmpty ? "Noch keine Gedanken festgehalten" : note.text)
                .font(.caption).foregroundStyle(.secondary).lineLimit(2)
        }
    }
}
struct MobileNoteEditor: View {
    @Binding var note: MobileNote
    @ObservedObject var browser: MobileBrowser
    var body: some View {
        Form {
            TextField("Titel", text: $note.title).listRowBackground(YOBROTheme.surface)
            TextEditor(text: $note.text).scrollContentBackground(.hidden)
                .frame(minHeight: 280).listRowBackground(YOBROTheme.surface)
            if !note.source.isEmpty {
                Text(note.source).font(.caption).textSelection(.enabled).listRowBackground(YOBROTheme.surface)
            }
        }.mobileSurface().navigationTitle("Notiz").onDisappear { browser.save() }
    }
}

struct MobileSettings: View {
    @EnvironmentObject private var account: MobileAccount
    @ObservedObject var browser: MobileBrowser
    @StateObject private var vpn = MobileVPN()
    @State private var confirmLogout = false
    @State private var showLogin = false
    var body: some View {
        Form {
            Section("Konto") {
                if account.isGuest {
                    Label("Ohne Konto unterwegs", systemImage: "person.crop.circle")
                    Text("Tabs, Notizen und Lesezeichen werden auf diesem Gerät gespeichert. Du brauchst dafür kein Konto.").font(.caption).foregroundStyle(.secondary)
                    NavigationLink { MobilePairingView() } label: {
                        Label("Mit Mac per QR verbinden", systemImage: "qrcode.viewfinder")
                    }
                    Button("Anmelden", systemImage: "person.crop.circle.badge.checkmark") { showLogin = true }
                } else if account.isPairedDevice {
                    Label("Mit deinem Mac verbunden", systemImage: "checkmark.icloud")
                    Text("Dieses iPhone hat keinen Konto-Login. Die Verbindung ist auf dein verschlüsseltes Browserprofil beschränkt.").font(.caption).foregroundStyle(.secondary)
                } else {
                    Text(account.session?.email ?? "")
                    if let message = account.message { Text(message).font(.caption).foregroundStyle(.secondary) }
                    NavigationLink("Desktop-Sync") { MobileSyncView(account: account, browser: browser) }
                    Text("Deine Daten ohne Konto bleiben separat erhalten. Nach dem Abmelden kehrst du dorthin zurück.").font(.caption).foregroundStyle(.secondary)
                    if let error = account.error { Text(error).font(.caption).foregroundStyle(.red) }
                    Button("Abmelden", role: .destructive) { confirmLogout = true }
                }
            }.listRowBackground(YOBROTheme.surface)
            Section("Webseiten") {
                Toggle("Dunkle Webseiten", isOn: $browser.state.darkWebsites).onChange(of: browser.state.darkWebsites) { _, _ in Task { await browser.settingsChanged() } }
                Toggle("Werbung und Tracker blockieren", isOn: $browser.state.blocking).onChange(of: browser.state.blocking) { _, _ in Task { await browser.settingsChanged() } }
                Text(browser.blockerStatus).font(.caption).foregroundStyle(.secondary)
                if #available(iOS 18.6, *), browser.extensions?.isReady == true {
                    NavigationLink("uBlock-Filterlisten und Einstellungen") { MobileExtensionOptions(runtime: browser.extensions!) }
                }
                Text("Ab iOS 18.6: uBlock Origin Lite mit gebündelten Filterlisten. Auf älteren Geräten: nativer Basis-Schutz. uBO Lite unterscheidet sich vom klassischen uBlock Origin.").font(.caption).foregroundStyle(.secondary)
            }
            .listRowBackground(YOBROTheme.surface).disabled(browser.protectionBusy)
            Section("VPN · IKEv2") {
                Text(vpn.status).foregroundStyle(vpn.connected ? .green : .secondary)
                TextField("VPN-Server", text: $vpn.server)
                TextField("Remote-ID des Servers", text: $vpn.remoteID)
                TextField("VPN-Benutzername", text: $vpn.username)
                SecureField("VPN-Passwort", text: $vpn.password)
                Button("VPN einrichten und verbinden") { Task { await vpn.connect() } }.disabled(vpn.busy)
                Button("VPN trennen") { vpn.disconnect() }
                Text("Benötigt einen IKEv2-Anbieter mit EAP-Zugangsdaten und Apples VPN-Freigabe. Deine YoBro-Anmeldung enthält keinen VPN-Dienst. Alternativ funktioniert der Browser mit deiner vorhandenen iOS-VPN-App.").font(.caption).foregroundStyle(.secondary)
            }.listRowBackground(YOBROTheme.surface).textInputAutocapitalization(.never).autocorrectionDisabled()

        }.mobileSurface().navigationTitle("Einstellungen").task { await vpn.load() }
            .sheet(isPresented: $showLogin) { MobileLogin().environmentObject(account).mobileSurface() }
            .confirmationDialog("Abmelden? Die lokalen Browserdaten bleiben für dieses Konto gespeichert. Eine aktive VPN-Verbindung bleibt bestehen.", isPresented: $confirmLogout, titleVisibility: .visible) {
                Button("Abmelden", role: .destructive) { browser.suspend(); Task { await account.signOut() } }
            }
    }
}

struct MobilePasswordReset: View {
    @EnvironmentObject private var account: MobileAccount
    @State private var password = ""
    @State private var confirmation = ""
    @State private var email = ""
    var body: some View {
        NavigationStack {
            Form {
                SecureField("Neues Passwort", text: $password).textContentType(.newPassword)
                SecureField("Passwort wiederholen", text: $confirmation).textContentType(.newPassword)
                if account.busy { ProgressView("Rücksetzlink wird verarbeitet …") }
                if let error = account.error { Text(error).foregroundStyle(.red) }
                if let message = account.message { Text(message).foregroundStyle(.secondary) }
                if !account.canUpdatePassword && !account.busy {
                    TextField("E-Mail-Adresse", text: $email).keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Neuen Rücksetzlink senden") { Task { await account.resetPassword(email: email) } }.disabled(email.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                Button("Passwort speichern") { Task { await account.changePassword(password); password = ""; confirmation = "" } }
                    .disabled(!account.canUpdatePassword || password.count < 8 || password != confirmation)
                Button("Abbrechen", role: .cancel) { account.cancelPasswordRecovery() }.disabled(account.busy)
            }.mobileSurface().navigationTitle("Neues Passwort")
                .onAppear { email = account.recoveryEmail }
                .onDisappear { password = ""; confirmation = "" }
        }
    }
}

@available(iOS 18.6, *)
struct MobileExtensionOptions: View {
    let runtime: MobileExtensions
    @State private var view: WKWebView?
    var body: some View {
        Group { if let view { MobileWebView(view: view) } else { ProgressView() } }
            .navigationTitle("uBlock Origin Lite").navigationBarTitleDisplayMode(.inline)
            .task { if view == nil { view = runtime.optionsView() } }
    }
}
