import SwiftUI

struct OnboardingProgress: Codable {
    var completed: Bool
    static func isComplete(home: URL) -> Bool {
        guard let data = try? Data(contentsOf: home.appendingPathComponent("onboarding.json")), let value = try? JSONDecoder().decode(Self.self, from:data) else { return false }
        return value.completed
    }
    static func finish(home: URL) throws {
        try JSONEncoder().encode(Self(completed:true)).write(to:home.appendingPathComponent("onboarding.json"), options:.atomic)
    }
}

extension BrowserModel {
    func finishOnboarding() {
        do {
            try OnboardingProgress.finish(home:home)
            showOnboarding = false
            openCommandBar(.newTab)
        }
        catch { notice = L("Die Einrichtung konnte nicht gespeichert werden: ") + error.localizedDescription }
    }
}

struct OnboardingTip: Identifiable {
    let icon: String
    let shortcut: String
    let title: String
    var id: String { icon }
}

/// Feature tips on the last onboarding step, two per row.
struct OnboardingTips: View {
    let tips: [OnboardingTip]
    var body: some View {
        LazyVGrid(columns:[GridItem(.flexible(),spacing:12),GridItem(.flexible(),spacing:12)],alignment:.leading,spacing:12) {
            ForEach(tips) { tip in
                HStack(spacing:12) {
                    Image(systemName:tip.icon).font(.system(size:15,weight:.semibold)).foregroundStyle(moss)
                        .frame(width:34,height:34).background(moss.opacity(0.12),in:RoundedRectangle(cornerRadius:9))
                    VStack(alignment:.leading,spacing:3) {
                        Text(tip.shortcut).font(.system(size:13,weight:.semibold,design:.rounded))
                        Text(tip.title).font(.system(size:12)).foregroundStyle(.secondary)
                    }
                    Spacer(minLength:0)
                }.yobroCard(padding:12)
            }
        }
    }
}

struct OnboardingView: View {
    @ObservedObject var model: BrowserModel
    @StateObject private var importer = BrowserImportStore()
    @StateObject private var defaultBrowser = DefaultBrowserSettings()
    @State var step = 0
    @State private var result = ""
    static var tips: [OnboardingTip] { [
        OnboardingTip(icon: "magnifyingglass", shortcut: "⌘T / ⌘K", title: L("Suchen oder Adresse", "Search or address")),
        OnboardingTip(icon: "rectangle.on.rectangle", shortcut: L("⇧-Klick auf Link", "⇧-click a link"), title: L("Vorschau mit Peek", "Preview with Peek")),
        OnboardingTip(icon: "rectangle.topthird.inset.filled", shortcut: "⌥⌘S", title: L("Tabs oben", "Tabs on top")),
        OnboardingTip(icon: "sparkles", shortcut: "⇧⌘A", title: L("Assistent", "Assistant"))
    ] }
    var body: some View {
        VStack(alignment: .leading, spacing:20) {
            HStack {
                YOBROMark(size:32)
                Text(L("Willkommen in YoBro")).font(.system(size:24,weight:.semibold))
                Spacer()
                Text("\(step + 1) / 4").font(.system(size:12,design:.monospaced)).foregroundStyle(.secondary)
            }
            Group {
                if step == 0 {
                    VStack(alignment:.leading,spacing:20) {
                        Text(L("Dein Browser. Dein Bro.", "Your browser. Your bro.")).font(.system(size:28,weight:.semibold))
                        Text(L("Nimm Lesezeichen, Verlauf und offene Tabs mit. Wähle selbst, ob du auch Passwörter und Cookies übertragen möchtest.")).font(.system(size:15)).lineSpacing(5)
                        HStack(spacing:18) { ForEach(ImportBrowser.all) { Text($0.name).font(.system(size:13,weight:.medium)) } }.foregroundStyle(moss)
                        Text(L("YoBro erkennt vorhandene Profile. Du siehst vor dem Import, was verfügbar ist. Deinen bisherigen Browser kannst du weiter nutzen.")).font(.system(size:13)).foregroundStyle(.secondary)
                        Spacer()
                    }.padding(.top,35)
                } else if step == 1 {
                    BrowserImportView(model:model, store:importer, onImported: { message in result = message; step = 2 })
                } else if step == 2 {
                    AgentConnectionsView(model: model)
                } else {
                    VStack(alignment:.leading,spacing:20) {
                        Label(L("Dein YoBro ist bereit"),systemImage:"checkmark.circle").font(.system(size:24,weight:.semibold))
                        Text(result.isEmpty ? L("Du kannst deine Browserdaten jederzeit in den Einstellungen importieren.") : result).font(.system(size:13)).textSelection(.enabled)
                        OnboardingTips(tips:Self.tips)
                        // Only on request: changing the default browser is a system-wide choice.
                        HStack(spacing:10) {
                            if defaultBrowser.isDefault {
                                Label(L("YoBro ist dein Standardbrowser", "YoBro is your default browser"),systemImage:"checkmark.circle.fill").foregroundStyle(moss)
                            } else {
                                Button(L("Als Standardbrowser festlegen", "Make YoBro your default browser")) { defaultBrowser.makeDefault() }
                                    .disabled(defaultBrowser.isChanging)
                                if defaultBrowser.isChanging { ProgressView().controlSize(.small) }
                            }
                        }.font(.system(size:13))
                        if let error = defaultBrowser.error { Text(error).font(.system(size:11)).foregroundStyle(.red) }
                        Spacer()
                    }.padding(.top,35)
                    .onAppear { defaultBrowser.refresh() }
                }
            }.frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.topLeading)
            Divider()
            HStack {
                if step == 1 { Button(L("Zurück")) { step = 0 }.disabled(importer.busy) }
                if step < 2 { Button(L("Vorerst überspringen")) { result = ""; step = 2 }.disabled(importer.busy) }
                Spacer()
                if step == 0 { Button(L("Browserdaten übertragen")) { step = 1 }.buttonStyle(.borderedProminent).tint(moss) }
                if step == 2 {
                    Button(L("Weiter", "Continue")) { step = 3 }.buttonStyle(.borderedProminent).tint(moss)
                }
                if step == 3 {
                    // The finished import cleared the preview; read the chosen profile again.
                    Button(L("Weitere Daten importieren")) { step = 1; Task { await importer.readProfile() } }
                    Button(L("Loslegen")) { model.finishOnboarding() }.buttonStyle(.borderedProminent).tint(moss)
                }
            }
        }.padding(28).frame(width:770,height:680).background(paper).foregroundStyle(ink)
            .textFieldStyle(YOBROTextFieldStyle()).tint(moss)
            .interactiveDismissDisabled()
    }
}
