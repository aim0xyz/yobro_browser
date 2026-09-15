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
            focusAddress = true
        }
        catch { notice = L("Die Einrichtung konnte nicht gespeichert werden: ") + error.localizedDescription }
    }
}

struct OnboardingView: View {
    @ObservedObject var model: BrowserModel
    @StateObject private var importer = BrowserImportStore()
    @State var step = 0
    @State private var result = ""
    var body: some View {
        VStack(alignment: .leading, spacing:20) {
            HStack {
                YOBROMark(size:32)
                Text(L("Willkommen in YoBro")).font(.system(size:28,design:.serif))
                Spacer()
                Text("\(step + 1) / 3").font(.system(size:12,design:.monospaced)).foregroundStyle(.secondary)
            }
            Group {
                if step == 0 {
                    VStack(alignment:.leading,spacing:20) {
                        Text(L("Dein Browser. Dein Bro.", "Your browser. Your bro.")).font(.system(size:30,design:.serif))
                        Text(L("Nimm Lesezeichen, Verlauf und offene Tabs mit. Wähle selbst, ob du auch Passwörter und Cookies übertragen möchtest.")).font(.system(size:15)).lineSpacing(5)
                        HStack(spacing:18) { ForEach(["Safari","Arc","Brave","Chrome","Firefox"],id:\.self) { Text($0).font(.system(size:13,weight:.medium)) } }.foregroundStyle(moss)
                        Text(L("YoBro erkennt vorhandene Profile. Du siehst vor dem Import, was verfügbar ist. Deinen bisherigen Browser kannst du weiter nutzen.")).font(.system(size:13)).foregroundStyle(.secondary)
                        Spacer()
                    }.padding(.top,35)
                } else if step == 1 {
                    BrowserImportView(model:model, store:importer, onImported: { message in result = message; step = 2 })
                } else {
                    VStack(alignment:.leading,spacing:20) {
                        Label(L("Dein YoBro ist bereit"),systemImage:"checkmark.circle").font(.system(size:26,design:.serif)).foregroundStyle(moss)
                        Text(result.isEmpty ? L("Du kannst deine Browserdaten jederzeit in den Einstellungen importieren.") : result).font(.system(size:13)).textSelection(.enabled)
                        Text(L("Mit ⌘T öffnest du einen Tab. Spaces und Ordner helfen dir beim Sortieren.")).font(.system(size:14)).foregroundStyle(.secondary)
                        Spacer()
                    }.padding(.top,35)
                }
            }.frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.topLeading)
            Divider()
            HStack {
                if step == 1 { Button(L("Zurück")) { step = 0 }.disabled(importer.busy) }
                if step < 2 { Button(L("Vorerst überspringen")) { result = ""; step = 2 }.disabled(importer.busy) }
                Spacer()
                if step == 0 { Button(L("Browserdaten übertragen")) { step = 1 }.buttonStyle(.borderedProminent).tint(moss) }
                if step == 2 {
                    Button(L("Weitere Daten importieren")) { step = 1 }
                    Button(L("Loslegen")) { model.finishOnboarding() }.buttonStyle(.borderedProminent).tint(moss)
                }
            }
        }.padding(28).frame(width:770,height:680).background(paper).foregroundStyle(ink)
            .textFieldStyle(YOBROTextFieldStyle()).tint(moss)
            .interactiveDismissDisabled()
    }
}
