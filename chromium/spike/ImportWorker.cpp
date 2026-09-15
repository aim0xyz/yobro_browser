#include "spike/ImportWorker.hpp"

#include "spike/Localization.hpp"
#include "spike/PythonWorker.hpp"

namespace yobro::spike {
namespace {

const QString kResource = QStringLiteral(":/yobro/BrowserImport.py");
const QString kFileName = QStringLiteral("BrowserImport.py");

PythonWorkerMessages importMessages() {
    PythonWorkerMessages messages;
    messages.missingInterpreter = L(
        QStringLiteral("Für den Import wird Python 3 benötigt. Installiere die "
                       "Xcode-Befehlszeilenwerkzeuge oder Python 3."),
        QStringLiteral("The import needs Python 3. Install the Xcode command line tools "
                       "or Python 3.")
    );
    messages.missingScript = L(QStringLiteral("Importmodul fehlt."),
                               QStringLiteral("The import module is missing."));
    messages.startFailed = L(QStringLiteral("Das Importmodul konnte nicht gestartet werden."),
                             QStringLiteral("The import module could not be started."));
    messages.timedOut = L(QStringLiteral("Das Importmodul hat nicht rechtzeitig geantwortet."),
                          QStringLiteral("The import module did not answer in time."));
    messages.badAnswer = L(QStringLiteral("Das Importmodul hat keine verwertbare Antwort geliefert."),
                           QStringLiteral("The import module did not return a usable answer."));
    messages.genericFailure = L(QStringLiteral("Der Import ist fehlgeschlagen."),
                                QStringLiteral("The import failed."));
    return messages;
}

} // namespace

QString ImportWorker::interpreter() {
    return PythonWorker::interpreter();
}

bool ImportWorker::available() {
    return !PythonWorker::interpreter().isEmpty()
        && !PythonWorker::scriptPath(kResource, kFileName).isEmpty();
}

ImportWorker::Result ImportWorker::call(const QJsonObject &request, int timeoutMilliseconds) {
    const PythonWorker::Result result =
        PythonWorker::call(kResource, kFileName, request, timeoutMilliseconds, importMessages());
    return {result.problem, result.value};
}

} // namespace yobro::spike
