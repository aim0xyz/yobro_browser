#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace yobro::spike {

/// Runs the import helper script that is shared with the WebKit build.
///
/// The script receives plaintext secrets: the Firefox primary password goes in
/// and decrypted logins come back. The interpreter is therefore resolved from a
/// fixed list of absolute paths instead of through `PATH`, and the environment
/// is trimmed, so a planted `python3` or an injected `PYTHONPATH` cannot get
/// hold of them.
class ImportWorker {
public:
    struct Result {
        /// Empty when the call succeeded.
        QString problem;
        QJsonValue value;
    };

    /// Sends `request` as JSON on stdin and reads the script's JSON answer.
    [[nodiscard]] static Result call(const QJsonObject &request, int timeoutMilliseconds = 60'000);

    /// The interpreter that would be used, or empty when none was found. Exposed
    /// so the shell can explain the situation instead of failing silently.
    [[nodiscard]] static QString interpreter();

    /// True when both an interpreter and the script are available.
    [[nodiscard]] static bool available();
};

} // namespace yobro::spike
