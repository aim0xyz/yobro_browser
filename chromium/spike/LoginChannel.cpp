#include "spike/LoginChannel.hpp"

#include <QJsonDocument>

namespace yobro::spike {

LoginAutofillChannel::LoginAutofillChannel(QObject *parent) : QObject(parent) {}

void LoginAutofillChannel::report(const QString &payload) {
    const QByteArray encoded = payload.toUtf8();
    if (encoded.isEmpty() || encoded.size() > maximumPayloadBytes)
        return;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;
    const QJsonObject event = document.object();
    if (event.isEmpty())
        return;
    Q_EMIT received(event);
}

} // namespace yobro::spike
