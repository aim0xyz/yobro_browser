#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace yobro::spike {

/// The wording a caller wants when the helper cannot run, so each feature can
/// explain the situation in its own words.
struct PythonWorkerMessages {
    QString missingInterpreter;
    QString missingScript;
    QString startFailed;
    QString timedOut;
    QString badAnswer;
    /// Used when the script reports a failure without a message of its own.
    QString genericFailure;
};

/// Runs one of the Python helper scripts that are shared with the WebKit build.
///
/// Both helpers handle plaintext secrets: a primary password or a mailbox
/// password goes in and readable data comes back. The interpreter is therefore
/// resolved from a fixed list of absolute paths instead of through `PATH`, and
/// the environment is trimmed, so a planted `python3` or an injected
/// `PYTHONPATH` cannot get hold of them.
class PythonWorker {
public:
    struct Result {
        /// Empty when the call succeeded.
        QString problem;
        QJsonValue value;
    };

    /// The interpreter that would be used, or empty when none was found.
    [[nodiscard]] static QString interpreter();

    /// Copies a script out of the Qt resource bundle into an owner-only file and
    /// returns its path. The copy is made once per process and reused.
    [[nodiscard]] static QString scriptPath(const QString &resource, const QString &fileName);

    /// Sends `request` as JSON on stdin and reads the script's JSON answer,
    /// which must be `{"ok": true, "result": …}` or `{"ok": false, "error": …}`.
    [[nodiscard]] static Result call(
        const QString &resource,
        const QString &fileName,
        const QJsonObject &request,
        int timeoutMilliseconds,
        const PythonWorkerMessages &messages
    );
};

} // namespace yobro::spike
