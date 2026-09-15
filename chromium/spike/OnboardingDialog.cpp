#include "spike/OnboardingDialog.hpp"

#include "spike/Localization.hpp"
#include "spike/OnboardingProgress.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <utility>

namespace yobro::spike {
namespace {

QLabel *paragraph(const QString &text, QWidget *parent, int size = 13, bool muted = false) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("font-size:%1px%2").arg(size).arg(
        muted ? QStringLiteral(";color:#7c7c7c") : QString()));
    return label;
}

} // namespace

OnboardingDialog::OnboardingDialog(
    std::filesystem::path profileDirectory,
    Importer importer,
    QWidget *parent
)
    : QDialog(parent), directory_(std::move(profileDirectory)), importer_(std::move(importer)) {
    setObjectName(QStringLiteral("onboardingDialog"));
    setWindowTitle(L(QStringLiteral("Willkommen in YoBro"), QStringLiteral("Welcome to YoBro")));
    setModal(true);
    resize(770, 680);

    finder_ = [](QString *problem) {
        QString reported;
        std::vector<ImportProfileEntry> entries = BrowserDataImport::profiles({}, reported);
        if (problem) *problem = reported;
        return entries;
    };
    reader_ = [](const QString &browser, const QString &path, const QStringList &kinds) {
        return BrowserDataImport::readProfile(browser, path, kinds);
    };

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 28);
    layout->setSpacing(20);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(L(QStringLiteral("Willkommen in YoBro"), QStringLiteral("Welcome to YoBro")), this);
    title->setObjectName(QStringLiteral("onboardingHeading"));
    title->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600"));
    counter_ = new QLabel(this);
    counter_->setObjectName(QStringLiteral("onboardingStepLabel"));
    counter_->setStyleSheet(QStringLiteral("font:11px monospace;color:#7c7c7c"));
    header->addWidget(title);
    header->addStretch();
    header->addWidget(counter_);
    layout->addLayout(header);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("onboardingPages"));

    // Step one: what the setup is for.
    auto *welcome = new QWidget(pages_);
    auto *welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->setContentsMargins(0, 0, 0, 0);
    welcomeLayout->setSpacing(16);
    auto *claim = new QLabel(L(QStringLiteral("Dein Browser. Dein Bro."),
                               QStringLiteral("Your browser. Your bro.")), welcome);
    claim->setStyleSheet(QStringLiteral("font-size:26px"));
    welcomeLayout->addWidget(claim);
    welcomeLayout->addWidget(paragraph(L(
        QStringLiteral("Nimm Lesezeichen, Verlauf und offene Tabs mit. Wähle selbst, ob du auch "
                       "Cookies übertragen möchtest."),
        QStringLiteral("Bring your bookmarks, history and open tabs along. You decide whether cookies "
                       "come too.")
    ), welcome, 15));
    // The same five names the WebKit build's welcome screen lists.
    auto *sources = new QLabel(QStringList({
        QStringLiteral("Safari"), QStringLiteral("Arc"), QStringLiteral("Brave"),
        QStringLiteral("Chrome"), QStringLiteral("Firefox"),
    }).join(QStringLiteral("   ·   ")), welcome);
    sources->setObjectName(QStringLiteral("onboardingSourceList"));
    sources->setStyleSheet(QStringLiteral("font-size:13px;font-weight:500;color:#536157"));
    welcomeLayout->addWidget(sources);
    welcomeLayout->addWidget(paragraph(L(
        QStringLiteral("YoBro erkennt vorhandene Profile. Du siehst vor dem Import, was verfügbar ist. "
                       "Deinen bisherigen Browser kannst du weiter nutzen."),
        QStringLiteral("YoBro finds existing profiles. You see what is available before importing. You "
                       "can keep using your current browser.")
    ), welcome, 13, true));
    // Passwords are deliberately not part of the first run: they need a Keychain
    // prompt, which does not belong into a welcome screen.
    welcomeLayout->addWidget(paragraph(L(
        QStringLiteral("Passwörter überträgst du später in den Einstellungen, weil dafür der "
                       "Schlüsselbund gefragt werden muss."),
        QStringLiteral("Passwords are transferred later in the settings, because that needs the "
                       "Keychain to be asked.")
    ), welcome, 13, true));
    welcomeLayout->addStretch();
    pages_->addWidget(welcome);

    // Step two: the actual transfer.
    auto *transferPage = new QWidget(pages_);
    auto *transferLayout = new QVBoxLayout(transferPage);
    transferLayout->setContentsMargins(0, 0, 0, 0);
    transferLayout->setSpacing(10);
    transferLayout->addWidget(paragraph(L(
        QStringLiteral("Profil wählen, Datenarten bestätigen, Vorschau ansehen und dann übernehmen."),
        QStringLiteral("Pick a profile, confirm the kinds of data, look at the preview and then transfer.")
    ), transferPage, 14));

    auto *pickerRow = new QHBoxLayout();
    profiles_ = new QComboBox(transferPage);
    profiles_->setObjectName(QStringLiteral("onboardingProfilePicker"));
    auto *findButton = new QPushButton(L(QStringLiteral("Profile suchen"),
                                         QStringLiteral("Find profiles")), transferPage);
    findButton->setObjectName(QStringLiteral("onboardingFindButton"));
    pickerRow->addWidget(profiles_, 1);
    pickerRow->addWidget(findButton);
    transferLayout->addLayout(pickerRow);

    auto *kindRow = new QHBoxLayout();
    for (const QString &kind : BrowserDataImport::kinds()) {
        auto *box = new QCheckBox(BrowserDataImport::kindLabel(kind), transferPage);
        box->setObjectName(QStringLiteral("onboardingKind-") + kind);
        // Cookies stay off: they carry live sessions and can sign the user out
        // of the browser they came from.
        box->setChecked(kind != QStringLiteral("cookies"));
        kindRow->addWidget(box);
        kinds_.emplace_back(kind, box);
    }
    kindRow->addStretch();
    transferLayout->addLayout(kindRow);

    preview_ = new QLabel(transferPage);
    preview_->setObjectName(QStringLiteral("onboardingPreview"));
    preview_->setWordWrap(true);
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    preview_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    transferLayout->addWidget(preview_, 1);

    auto *transferButtons = new QHBoxLayout();
    previewButton_ = new QPushButton(L(QStringLiteral("Vorschau"), QStringLiteral("Preview")), transferPage);
    previewButton_->setObjectName(QStringLiteral("onboardingPreviewButton"));
    importButton_ = new QPushButton(L(QStringLiteral("Daten übernehmen"),
                                      QStringLiteral("Transfer data")), transferPage);
    importButton_->setObjectName(QStringLiteral("onboardingImportButton"));
    importButton_->setEnabled(false);
    transferButtons->addWidget(previewButton_);
    transferButtons->addWidget(importButton_);
    transferButtons->addStretch();
    transferLayout->addLayout(transferButtons);
    pages_->addWidget(transferPage);

    // Step three: what happened, and how to get going.
    auto *donePage = new QWidget(pages_);
    auto *doneLayout = new QVBoxLayout(donePage);
    doneLayout->setContentsMargins(0, 0, 0, 0);
    doneLayout->setSpacing(16);
    auto *doneTitle = new QLabel(L(QStringLiteral("Dein YoBro ist bereit"),
                                   QStringLiteral("Your YoBro is ready")), donePage);
    doneTitle->setStyleSheet(QStringLiteral("font-size:22px;color:#536157"));
    doneLayout->addWidget(doneTitle);
    resultLabel_ = new QLabel(donePage);
    resultLabel_->setObjectName(QStringLiteral("onboardingResult"));
    resultLabel_->setWordWrap(true);
    resultLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    doneLayout->addWidget(resultLabel_);
    doneLayout->addWidget(paragraph(L(
        QStringLiteral("Mit ⌘T öffnest du einen Tab. Spaces und Ordner helfen dir beim Sortieren."),
        QStringLiteral("⌘T opens a tab. Spaces and folders help you sort things.")
    ), donePage, 14, true));
    doneLayout->addStretch();
    pages_->addWidget(donePage);
    layout->addWidget(pages_, 1);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);

    auto *footer = new QHBoxLayout();
    backButton_ = new QPushButton(L(QStringLiteral("Zurück"), QStringLiteral("Back")), this);
    backButton_->setObjectName(QStringLiteral("onboardingBackButton"));
    skipButton_ = new QPushButton(L(QStringLiteral("Vorerst überspringen"),
                                    QStringLiteral("Skip for now")), this);
    skipButton_->setObjectName(QStringLiteral("onboardingSkipButton"));
    transferStepButton_ = new QPushButton(L(QStringLiteral("Browserdaten übertragen"),
                                            QStringLiteral("Transfer browser data")), this);
    transferStepButton_->setObjectName(QStringLiteral("onboardingTransferButton"));
    moreButton_ = new QPushButton(L(QStringLiteral("Weitere Daten importieren"),
                                    QStringLiteral("Import more data")), this);
    moreButton_->setObjectName(QStringLiteral("onboardingMoreButton"));
    startButton_ = new QPushButton(L(QStringLiteral("Loslegen"), QStringLiteral("Get started")), this);
    startButton_->setObjectName(QStringLiteral("onboardingStartButton"));
    startButton_->setDefault(true);
    footer->addWidget(backButton_);
    footer->addWidget(skipButton_);
    footer->addStretch();
    footer->addWidget(transferStepButton_);
    footer->addWidget(moreButton_);
    footer->addWidget(startButton_);
    layout->addLayout(footer);

    QObject::connect(backButton_, &QPushButton::clicked, this, [this] { showStep(0); });
    QObject::connect(skipButton_, &QPushButton::clicked, this, [this] {
        result_.clear();
        showStep(2);
    });
    QObject::connect(transferStepButton_, &QPushButton::clicked, this, [this] { showStep(1); });
    QObject::connect(moreButton_, &QPushButton::clicked, this, [this] { showStep(1); });
    QObject::connect(startButton_, &QPushButton::clicked, this, [this] { finish(); });
    QObject::connect(findButton, &QPushButton::clicked, this, [this] { findProfiles(); });
    QObject::connect(previewButton_, &QPushButton::clicked, this, [this] { preview(); });
    QObject::connect(importButton_, &QPushButton::clicked, this, [this] { transfer(); });

    showStep(0);
}

void OnboardingDialog::setProfileFinder(ProfileFinder finder) {
    finder_ = std::move(finder);
}

void OnboardingDialog::setProfileReader(ProfileReader reader) {
    reader_ = std::move(reader);
}

void OnboardingDialog::showStep(int step) {
    step_ = std::max(0, std::min(2, step));
    pages_->setCurrentIndex(step_);
    counter_->setText(QStringLiteral("%1 / 3").arg(step_ + 1));

    backButton_->setVisible(step_ == 1);
    skipButton_->setVisible(step_ < 2);
    transferStepButton_->setVisible(step_ == 0);
    moreButton_->setVisible(step_ == 2);
    // Only the last step can finish the setup, so nobody lands in the browser
    // with the setup still marked as unfinished.
    startButton_->setVisible(step_ == 2);

    if (step_ == 1 && profiles_->count() == 0) findProfiles();
    if (step_ == 2) {
        resultLabel_->setText(result_.isEmpty()
            ? L(QStringLiteral("Du kannst deine Browserdaten jederzeit in den Einstellungen importieren."),
                QStringLiteral("You can import your browser data at any time from the settings."))
            : result_.join(QStringLiteral("\n")));
    }
}

QStringList OnboardingDialog::wantedKinds() const {
    QStringList wanted;
    for (const auto &entry : kinds_)
        if (entry.second->isChecked()) wanted.append(entry.first);
    return wanted;
}

void OnboardingDialog::findProfiles() {
    QString problem;
    found_ = finder_(&problem);
    const QSignalBlocker blocker(profiles_);
    profiles_->clear();
    for (const ImportProfileEntry &entry : found_) {
        profiles_->addItem(QStringLiteral("%1 · %2").arg(
            entry.browser, entry.name.isEmpty() ? entry.path : entry.name));
    }
    previewed_.reset();
    importButton_->setEnabled(false);
    if (!problem.isEmpty()) {
        preview_->setText(problem);
        return;
    }
    preview_->setText(found_.empty()
        ? L(QStringLiteral("Keine Profile anderer Browser gefunden."),
            QStringLiteral("No profiles of other browsers were found."))
        : L(QStringLiteral("Profil wählen und Vorschau ansehen."),
            QStringLiteral("Pick a profile and look at the preview.")));
}

void OnboardingDialog::preview() {
    const int index = profiles_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= found_.size()) {
        preview_->setText(L(QStringLiteral("Bitte zuerst ein Profil suchen und auswählen."),
                            QStringLiteral("Please find and select a profile first.")));
        return;
    }
    const QStringList wanted = wantedKinds();
    if (wanted.isEmpty()) {
        previewed_.reset();
        importButton_->setEnabled(false);
        preview_->setText(L(QStringLiteral("Mindestens eine Datenart wählen."),
                            QStringLiteral("Choose at least one kind of data.")));
        return;
    }

    const ImportProfileEntry &profile = found_.at(static_cast<std::size_t>(index));
    ImportedBrowserData data = reader_(profile.browser, profile.path, wanted);
    if (!data.problem.isEmpty()) {
        previewed_.reset();
        importButton_->setEnabled(false);
        preview_->setText(data.problem);
        return;
    }

    QStringList lines;
    for (const QString &kind : BrowserDataImport::kinds()) {
        if (!wanted.contains(kind)) continue;
        const std::size_t count = kind == QStringLiteral("bookmarks") ? data.bookmarks.size()
            : kind == QStringLiteral("history") ? data.history.size()
            : kind == QStringLiteral("tabs") ? data.tabs.size()
            : data.cookies.size();
        QString line = QStringLiteral("%1: %2").arg(BrowserDataImport::kindLabel(kind)).arg(count);
        if (const QString note = data.availability.value(kind); !note.isEmpty())
            line += QStringLiteral(" · ") + note;
        lines.append(line);
    }
    lines.append(data.warnings);
    lines.append(L(
        QStringLiteral("Die Quelle bleibt unverändert. Erst „Daten übernehmen“ schreibt in dieses Profil."),
        QStringLiteral("The source stays unchanged. Only “Transfer data” writes into this profile.")
    ));
    preview_->setText(lines.join(QStringLiteral("\n")));
    const bool anything = data.total() > 0;
    previewed_ = std::move(data);
    importButton_->setEnabled(anything);
}

void OnboardingDialog::transfer() {
    if (!previewed_ || !importer_) return;
    QString problem;
    const QStringList summary = importer_(*previewed_, &problem);
    if (!problem.isEmpty()) {
        preview_->setText(problem);
        return;
    }
    // The preview is spent: a second transfer would duplicate everything.
    previewed_.reset();
    importButton_->setEnabled(false);
    result_ = summary;
    showStep(2);
}

void OnboardingDialog::finish() {
    if (const QString problem = OnboardingProgress::finish(directory_); !problem.isEmpty()) {
        // The window stays usable, but the setup will ask again next time, and
        // the user should know that rather than be surprised.
        resultLabel_->setText(problem);
        return;
    }
    Q_EMIT completed();
    accept();
}

} // namespace yobro::spike
