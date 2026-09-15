#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::showOnboarding() {
    if (onboardingDialog_) {
        onboardingDialog_->show();
        onboardingDialog_->raise();
        onboardingDialog_->activateWindow();
        return;
    }
    // The setup writes through the same path as the settings dialog, so both
    // ways into an import behave identically.
    auto *dialog = new OnboardingDialog(
        paths_.profile,
        [this](const ImportedBrowserData &data, QString *problem) {
            return applyImportedBrowserData(data, problem);
        },
        this
    );
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()));
    QObject::connect(dialog, &OnboardingDialog::completed, this, [this] {
        showStatus(L(QStringLiteral("Einrichtung abgeschlossen."), QStringLiteral("Setup finished.")));
        // The WebKit build focuses the address bar afterwards.
        if (address_) {
            address_->setFocus(Qt::OtherFocusReason);
            address_->selectAll();
        }
    });
    onboardingDialog_ = dialog;
    dialog->show();
}

} // namespace yobro::spike
