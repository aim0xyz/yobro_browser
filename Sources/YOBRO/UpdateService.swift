import Foundation
import Sparkle
import SwiftUI

/// Sparkle owns downloading, signature verification, replacement, and relaunch.
/// This wrapper deliberately performs background *information* checks only. The
/// user chooses installation from YoBro's sidebar or the Browser menu.
@MainActor
final class UpdateService: NSObject, ObservableObject, SPUUpdaterDelegate {
    @Published private(set) var availableVersion: String?
    @Published private(set) var checking = false
    @Published private(set) var status: String?

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

    func checkSilently() {
        guard let controller, controller.updater.canCheckForUpdates, !checking else { return }
        checking = true
        status = nil
        controller.updater.checkForUpdateInformation()
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
        availableVersion = nil
        checking = false
        // Sparkle uses this callback for both the normal "already current"
        // result and a rejected update. Neither needs a sidebar interruption.
        status = nil
    }

    func updater(_ updater: SPUUpdater, didFinishUpdateCycleFor updateCheck: SPUUpdateCheck, error: (any Error)?) {
        checking = false
        if let error { status = error.localizedDescription }
    }
}

struct UpdateSidebarCard: View {
    @ObservedObject var updates: UpdateService

    var body: some View {
        if let version = updates.availableVersion {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 7) {
                    Image(systemName: "arrow.down.circle.fill").foregroundStyle(moss)
                    Text(L("Update verfügbar", "Update available")).font(.system(size: 11, weight: .semibold))
                    Spacer()
                }
                Text(L("YoBro \(version) kann installiert werden.", "YoBro \(version) is ready to install."))
                    .font(.system(size: 10)).foregroundStyle(ink.opacity(0.62))
                Button(L("Update installieren", "Install update")) { updates.installAvailableUpdate() }
                    .buttonStyle(.borderedProminent).tint(moss).controlSize(.small)
            }
            .padding(12)
            .background(moss.opacity(0.10), in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(moss.opacity(0.20)))
            .padding(.bottom, 8)
            .accessibilityElement(children: .contain)
        }
    }
}
