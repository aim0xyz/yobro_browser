#include "spike/MailWorker.hpp"

#include "spike/Localization.hpp"
#include "spike/PythonWorker.hpp"

#include <QCoreApplication>
#include <QPointer>

#include <thread>
#include <utility>

namespace yobro::spike {
namespace {

const QString kResource = QStringLiteral(":/yobro/MailWorker.py");
const QString kFileName = QStringLiteral("MailWorker.py");

PythonWorkerMessages mailMessages() {
    PythonWorkerMessages messages;
    messages.missingInterpreter = L(
        QStringLiteral("Für Mail wird Python 3 benötigt. Installiere die "
                       "Xcode-Befehlszeilenwerkzeuge oder Python 3."),
        QStringLiteral("Mail needs Python 3. Install the Xcode command line tools or Python 3.")
    );
    messages.missingScript = L(QStringLiteral("Mail-Komponente fehlt."),
                               QStringLiteral("The mail component is missing."));
    messages.startFailed = L(QStringLiteral("Die Mail-Komponente konnte nicht gestartet werden."),
                             QStringLiteral("The mail component could not be started."));
    messages.timedOut = L(QStringLiteral("Mail-Verbindung beendet oder Zeitlimit erreicht."),
                          QStringLiteral("The mail connection ended or timed out."));
    messages.badAnswer = L(QStringLiteral("Ungültige Mail-Antwort."),
                           QStringLiteral("Invalid mail response."));
    messages.genericFailure = L(QStringLiteral("Mail-Verbindung fehlgeschlagen."),
                                QStringLiteral("The mail connection failed."));
    return messages;
}

} // namespace

QString MailWorker::interpreter() {
    return PythonWorker::interpreter();
}

bool MailWorker::available() {
    return !PythonWorker::interpreter().isEmpty()
        && !PythonWorker::scriptPath(kResource, kFileName).isEmpty();
}

MailWorker::Result MailWorker::call(const QJsonObject &request, int timeoutMilliseconds) {
    const PythonWorker::Result result =
        PythonWorker::call(kResource, kFileName, request, timeoutMilliseconds, mailMessages());
    return {result.problem, result.value.toObject()};
}

void MailWorker::callAsync(
    const QJsonObject &request,
    int timeoutMilliseconds,
    QObject *context,
    std::function<void(Result)> done
) {
    // The script copy is made here, on the calling thread, so two mail calls
    // starting at once cannot race over the extracted file.
    (void)PythonWorker::scriptPath(kResource, kFileName);
    const QPointer<QObject> guard(context);
    std::thread([request, timeoutMilliseconds, guard, done = std::move(done)]() mutable {
        Result result = call(request, timeoutMilliseconds);
        // The answer is posted to the application object, which always outlives
        // the window, and only then is the receiver checked. That check happens
        // on the main thread, where the pointer is also cleared, so a mailbox
        // answer arriving after the window closed is simply dropped.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, done = std::move(done), result = std::move(result)]() {
                if (!guard) return;
                done(result);
            },
            Qt::QueuedConnection
        );
    }).detach();
}

} // namespace yobro::spike
