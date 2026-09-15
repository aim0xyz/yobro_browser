#include "spike/Localization.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

QJsonObject readTable(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "A translation table is missing from the resources.");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    check(document.isObject(), "A translation table is not a JSON object.");
    return document.object();
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    try {
        using yobro::spike::L;

        // Both tables must be reachable through the Qt resources.
        const QJsonObject shared = readTable(QStringLiteral(":/yobro/English.json"));
        const QJsonObject chromium = readTable(QStringLiteral(":/yobro/EnglishChromium.json"));
        check(!shared.isEmpty(), "The shared translation table is empty.");
        check(!chromium.isEmpty(), "The Chromium translation table is empty.");

        // Every value must be a non-empty string, otherwise a label would vanish.
        for (const QJsonObject &table : {shared, chromium}) {
            for (auto it = table.begin(); it != table.end(); ++it) {
                check(it.value().isString(), "A translation is not a string.");
                check(!it.key().isEmpty(), "A translation key is empty.");
                check(!it.value().toString().isEmpty(), "A translation is empty.");
            }
        }

        // Placeholders must survive translation, or interpolation breaks.
        for (auto it = chromium.begin(); it != chromium.end(); ++it) {
            for (const QString &placeholder : {QStringLiteral("%1"), QStringLiteral("%2"), QStringLiteral("%3")}) {
                check(it.key().contains(placeholder) == it.value().toString().contains(placeholder),
                      "A translation lost or invented a placeholder.");
            }
        }

        // The Chromium table must not silently disagree with the shared one.
        for (auto it = chromium.begin(); it != chromium.end(); ++it) {
            if (!shared.contains(it.key())) continue;
            check(shared.value(it.key()).toString() == it.value().toString(),
                  "The Chromium table contradicts the shared translation.");
        }

        // The harness passes the language it configured, so the selection logic
        // itself is verified rather than assumed.
        if (argc > 1) {
            const QString expected = QString::fromLatin1(argv[1]);
            if (expected == QStringLiteral("de"))
                check(yobro::spike::isGerman(), "The German override was not honoured.");
            else if (expected == QStringLiteral("en"))
                check(!yobro::spike::isGerman(), "The English override was not honoured.");
            else
                fail("Unknown language expectation.");
            const QString sample = QStringLiteral("Neuer Tab");
            const QString translated = L(sample);
            check(expected == QStringLiteral("de")
                      ? translated == sample
                      : translated != sample,
                  "A known label did not follow the configured language.");
        }

        // The explicit pair always answers, whatever the system language is.
        const QString german = QStringLiteral("Neuer Tab");
        const QString english = QStringLiteral("New tab");
        const QString paired = L(german, english);
        check(paired == (yobro::spike::isGerman() ? german : english),
              "The bilingual form did not follow the system language.");

        // A lookup with no entry must fall back instead of returning nothing.
        const QString unknown = QStringLiteral("Dieser Text ist absichtlich nicht übersetzt");
        check(L(unknown) == unknown, "A missing translation must fall back to the German text.");

        // A known key resolves for both languages.
        const QString knownKey = chromium.begin().key();
        const QString resolved = L(knownKey);
        check(resolved == (yobro::spike::isGerman() ? knownKey : chromium.value(knownKey).toString()),
              "A known key did not resolve for the current language.");
        check(!resolved.isEmpty(), "A resolved translation must not be empty.");
    } catch (const std::exception &error) {
        std::cerr << "LOCALIZATION FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "LOCALIZATION PASS\n";
    return 0;
}
