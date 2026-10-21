import Foundation
import Sparkle
import SwiftUI

/// Sparkle owns downloading, signature verification, replacement, and relaunch.
/// One updater serves the whole app, however many profiles are open. Scheduled
/// checks present Sparkle's own update window; a found version also shows in
/// the sidebar, and the Browser menu starts a check on demand.
@MainActor
final class UpdateService: NSObject, ObservableObject, SPUUpdaterDelegate {
    static let shared = UpdateService()

    @Published private(set) var availableVersion: String?
    @Published private(set) var checking = false

    private var controller: SPUStandardUpdaterController?
    private var started = false

    var isConfigured: Bool {
        guard let value = Bundle.main.object(forInfoDictionaryKey: "SUFeedURL") as? String,
              let url = URL(string: value), url.scheme == "https", !url.host!.isEmpty else { return false }
        return true
    }

    func start() {
        guard isConfigured, !started else { return }
        started = true
        controller = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: self, userDriverDelegate: nil)
    }

    func installAvailableUpdate() {
        guard let controller, controller.updater.canCheckForUpdates else { return }
        // The standard Sparkle UI presents release notes and requires the user's
        // explicit choice before downloading or replacing the application.
        controller.checkForUpdates(nil)
    }

    func updater(_ updater: SPUUpdater, didFindValidUpdate item: SUAppcastItem) {
        availableVersion = item.displayVersionString
        checking = false
    }

    func updaterDidNotFindUpdate(_ updater: SPUUpdater, error: any Error) {
        // Sparkle uses this callback for both the normal "already current"
        // result and a rejected update. Neither needs a sidebar interruption.
        availableVersion = nil
        checking = false
    }

    func updater(_ updater: SPUUpdater, didFinishUpdateCycleFor updateCheck: SPUUpdateCheck, error: (any Error)?) {
        checking = false
    }
}

struct UpdateSidebarCard: View {
    @ObservedObject var updates: UpdateService
    @Environment(\.spaceTone) private var tone

    var body: some View {
        if let version = updates.availableVersion {
            Button { updates.installAvailableUpdate() } label: {
                HStack(spacing: 8) {
                    Image(systemName: "arrow.down.circle.fill")
                    Text(L("YoBro \(version) installieren", "Install YoBro \(version)")).font(.system(size: 12.5, weight: .medium))
                    Spacer()
                }
                .foregroundStyle(tone.muted)
                .padding(.horizontal, 10).frame(height: Chrome.rowHeight)
                .background(Chrome.rest, in: RoundedRectangle(cornerRadius: Chrome.radius))
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.top, 6)
            .accessibilityElement(children: .contain)
        }
    }
}
