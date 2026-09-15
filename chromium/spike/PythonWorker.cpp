#include "spike/PythonWorker.hpp"

#include "spike/Localization.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace yobro::spike {
namespace {

/// Absolute interpreter paths, preferring the one that ships with macOS. Same
/// list and same reasoning as the WebKit build.
const QStringList &interpreterCandidates() {
    static const QStringList candidates{
        QStringLiteral("/usr/bin/python3"),
        QStringLiteral("/opt/homebrew/bin/python3"),
        QStringLiteral("/usr/local/bin/python3"),
    };
    return candidates;
}

QProcessEnvironment workerEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin:/usr/sbin:/sbin"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONSTARTUP"));
    // The scripts localise their own messages the same way the app does.
    environment.insert(
        QStringLiteral("YOBRO_LANGUAGE"),
        isGerman() ? QStringLiteral("de") : QStringLiteral("en")
    );
    return environment;
}

} // namespace

QString PythonWorker::interpreter() {
    for (const QString &candidate : interpreterCandidates()) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return candidate;
    }
    return {};
}

QString PythonWorker::scriptPath(const QString &resource, const QString &fileName) {
    // One extracted copy per script, kept for the lifetime of the process.
    static QHash<QString, QString> paths;
    static QTemporaryDir *directory = nullptr;
    if (const auto found = paths.constFind(resource); found != paths.constEnd()) return *found;

    QFile source(resource);
    if (!source.open(QIODevice::ReadOnly)) return {};
    if (!directory) directory = new QTemporaryDir();
    if (!directory->isValid()) return {};
    const QString target = directory->filePath(fileName);
    QFile copy(target);
    if (!copy.open(QIODevice::WriteOnly)) return {};
    copy.write(source.readAll());
    copy.close();
    if (!copy.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return {};
    paths.insert(resource, target);
    return target;
}

PythonWorker::Result PythonWorker::call(
    const QString &resource,
    const QString &fileName,
    const QJsonObject &request,
    int timeoutMilliseconds,
    const PythonWorkerMessages &messages
) {
    const QString python = interpreter();
    if (python.isEmpty()) return {messages.missingInterpreter, {}};
    const QString script = scriptPath(resource, fileName);
    if (script.isEmpty()) return {messages.missingScript, {}};

    QProcess process;
    process.setProgram(python);
    process.setArguments({script});
    process.setProcessEnvironment(workerEnvironment());
    process.start();
    if (!process.waitForStarted(10'000)) return {messages.startFailed, {}};
    process.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMilliseconds)) {
        process.kill();
        process.waitForFinished(2'000);
        return {messages.timedOut, {}};
    }

    const QJsonObject answer = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    if (answer.isEmpty()) return {messages.badAnswer, {}};
    if (!answer.value(QStringLiteral("ok")).toBool()) {
        const QString reported = answer.value(QStringLiteral("error")).toString();
        return {reported.isEmpty() ? messages.genericFailure : reported, {}};
    }
    return {{}, answer.value(QStringLiteral("result"))};
}

} // namespace yobro::spike
