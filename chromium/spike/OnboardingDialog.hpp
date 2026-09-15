#pragma once

#include "spike/BrowserDataImport.hpp"

#include <QDialog>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace yobro::spike {

/// The three-step first-run setup, mirroring `Onboarding.swift`.
///
/// Step two is the real data transfer, not a placeholder: it uses the same
/// reader and the same writer as the settings dialog, so nothing can drift
/// between the two ways into the same operation.
class OnboardingDialog final : public QDialog {
    Q_OBJECT

public:
    /// Writes imported data into the profile and returns what happened, one
    /// sentence per line. Supplied by the window, which owns the library and the
    /// tabs the data goes into.
    using Importer = std::function<QStringList(const ImportedBrowserData &data, QString *problem)>;
    /// Finds the profiles of other browsers. Replaceable for tests.
    using ProfileFinder = std::function<std::vector<ImportProfileEntry>(QString *problem)>;
    /// Reads one profile without writing anything. Replaceable for tests.
    using ProfileReader = std::function<ImportedBrowserData(
        const QString &browser,
        const QString &path,
        const QStringList &kinds
    )>;

    OnboardingDialog(
        std::filesystem::path profileDirectory,
        Importer importer,
        QWidget *parent = nullptr
    );

    void setProfileFinder(ProfileFinder finder);
    void setProfileReader(ProfileReader reader);

    /// 0, 1 or 2. Exposed so a harness can check where the user is.
    [[nodiscard]] int step() const { return step_; }

Q_SIGNALS:
    /// The setup was finished and recorded. The window should focus the address
    /// bar, as the WebKit build does.
    void completed();

private:
    void showStep(int step);
    void findProfiles();
    void preview();
    void transfer();
    void finish();
    [[nodiscard]] QStringList wantedKinds() const;

    std::filesystem::path directory_;
    Importer importer_;
    ProfileFinder finder_;
    ProfileReader reader_;

    QStackedWidget *pages_ = nullptr;
    QLabel *counter_ = nullptr;
    QLabel *resultLabel_ = nullptr;
    QLabel *preview_ = nullptr;
    QComboBox *profiles_ = nullptr;
    QPushButton *backButton_ = nullptr;
    QPushButton *skipButton_ = nullptr;
    QPushButton *transferStepButton_ = nullptr;
    QPushButton *previewButton_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *moreButton_ = nullptr;
    QPushButton *startButton_ = nullptr;
    std::vector<std::pair<QString, QCheckBox *>> kinds_;
    std::vector<ImportProfileEntry> found_;
    /// The last preview, so the transfer writes exactly what was shown.
    std::optional<ImportedBrowserData> previewed_;
    QStringList result_;
    int step_ = 0;
};

} // namespace yobro::spike
