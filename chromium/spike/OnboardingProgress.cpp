#include "spike/OnboardingProgress.hpp"

#include "spike/Localization.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <system_error>

namespace yobro::spike {
namespace {

const QString kCompletedKey = QStringLiteral("completed");

QString filePath(const std::filesystem::path &profileDirectory) {
    return QString::fromStdString((profileDirectory / "onboarding.json").string());
}

} // namespace

bool OnboardingProgress::isComplete(const std::filesystem::path &profileDirectory) {
    QFile file(filePath(profileDirectory));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return false;
    return document.object().value(kCompletedKey).toBool(false);
}

QString OnboardingProgress::finish(const std::filesystem::path &profileDirectory) {
    std::error_code code;
    std::filesystem::create_directories(profileDirectory, code);
    QJsonObject root;
    root.insert(kCompletedKey, true);
    QSaveFile file(filePath(profileDirectory));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        return L(QStringLiteral("Die Einrichtung konnte nicht gespeichert werden."),
                 QStringLiteral("The setup could not be saved."));
    }
    return {};
}

QString OnboardingProgress::reset(const std::filesystem::path &profileDirectory) {
    QFile file(filePath(profileDirectory));
    if (!file.exists()) return {};
    if (!file.remove()) {
        return L(QStringLiteral("Die Einrichtung konnte nicht zurückgesetzt werden."),
                 QStringLiteral("The setup could not be reset."));
    }
    return {};
}

} // namespace yobro::spike
