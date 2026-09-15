#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace yobro::spike {

/// Receives login events pushed out of the isolated LoginAutofill world.
///
/// The WebKit build registers a WKScriptMessageHandler, so the page reports the
/// moment something happens. Qt WebEngine has no message handler, and this
/// build used to poll `takeEvent()` every 350 ms instead: a repeated JavaScript
/// evaluation on every https page the user had in front of them, which both
/// delayed the suggestion popup and kept waking the renderer for nothing.
/// QWebChannel is the supported push path, and it can be bound to a single
/// JavaScript world, so page scripts still cannot see or call this object.
class LoginAutofillChannel : public QObject {
    Q_OBJECT
public:
    explicit LoginAutofillChannel(QObject *parent = nullptr);

    /// Called from the isolated world with one JSON object. Anything that is
    /// not a small, well-formed object is dropped without a signal, because the
    /// payload carries credentials and must never be guessed at.
    Q_INVOKABLE void report(const QString &payload);

    /// Upper bound for one reported event. A username is capped at 1024 and a
    /// password at 4096 characters further down, so this only has to stop a
    /// compromised renderer from pushing something huge at the shell.
    static constexpr int maximumPayloadBytes = 16384;

Q_SIGNALS:
    void received(const QJsonObject &event);
};

} // namespace yobro::spike
