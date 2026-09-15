#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

namespace yobro::spike {

/// Runs `MailWorker.py`, the IMAP and SMTP helper shared with the WebKit build.
///
/// Reimplementing IMAP and SMTP in C++ would mean a second, differently
/// behaving mail client. The script is therefore shared verbatim, exactly like
/// the import helper, so both shells talk to a provider the same way.
class MailWorker {
public:
    struct Result {
        /// Empty when the call succeeded.
        QString problem;
        QJsonObject value;
    };

    /// Blocking call. Only for tests and for the short `test` handshake.
    [[nodiscard]] static Result call(const QJsonObject &request, int timeoutMilliseconds = 90'000);

    /// Runs the helper on its own thread and answers on the thread that called.
    ///
    /// A mailbox round trip takes seconds, so the window must not wait for it.
    /// `context` decides how long the answer stays interesting: when it is gone,
    /// the answer is dropped instead of reaching a deleted receiver.
    static void callAsync(
        const QJsonObject &request,
        int timeoutMilliseconds,
        QObject *context,
        std::function<void(Result)> done
    );

    [[nodiscard]] static bool available();
    [[nodiscard]] static QString interpreter();
};

} // namespace yobro::spike
