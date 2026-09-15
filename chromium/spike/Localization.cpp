#include "spike/Localization.hpp"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace yobro::spike {
namespace {

bool determineGerman() {
    // An explicit override keeps automated checks independent of the machine's
    // system language. Everything else follows the system.
    const QByteArray requested = qgetenv("YOBRO_LANGUAGE");
    if (!requested.isEmpty())
        return QString::fromUtf8(requested).toLower().startsWith(QStringLiteral("de"));
    const QStringList languages = QLocale::system().uiLanguages();
    QString primary = languages.isEmpty()
        ? QLocale::system().name()
        : languages.first();
    // Only the primary language counts, and "de-AT" or "de_CH" count as German.
    primary.replace(QLatin1Char('_'), QLatin1Char('-'));
    return primary.toLower().startsWith(QStringLiteral("de"));
}

QHash<QString, QString> loadTranslations() {
    QHash<QString, QString> values;
    // The shared file first, then the Chromium-only additions.
    for (const QString &path : {QStringLiteral(":/yobro/English.json"),
                               QStringLiteral(":/yobro/EnglishChromium.json")}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!it.value().isString()) continue;
            values.insert(it.key(), it.value().toString());
        }
    }
    return values;
}

} // namespace

bool isGerman() {
    static const bool german = determineGerman();
    return german;
}

QString L(const QString &german) {
    if (isGerman()) return german;
    static const QHash<QString, QString> translations = loadTranslations();
    const auto found = translations.constFind(german);
    return found == translations.constEnd() ? german : *found;
}

QString L(const QString &german, const QString &english) {
    return isGerman() ? german : english;
}

} // namespace yobro::spike
