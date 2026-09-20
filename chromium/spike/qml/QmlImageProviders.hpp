#pragma once

#include <QQuickImageProvider>

class QColor;

namespace yobro::spike {

/// Tinted Lucide chrome icons for QML, rendered from the same `:/yobro/icons`
/// set the widget shell embeds. `Theme.iconUrl(name, role)` builds the URLs.
class QmlIconProvider : public QQuickImageProvider {
public:
    QmlIconProvider();

    QImage requestImage(
        const QString &id,
        QSize *size,
        const QSize &requestedSize
    ) override;
};

/// The start page's radial moss glow as an on-demand texture, so QML shows the
/// exact `RadialGradient(center, 20, 420)` the WebKit start page draws without
/// pulling a shader dependency into the shell. Sized via `image://yobroglow/WxH`.
class QmlGlowProvider : public QQuickImageProvider {
public:
    QmlGlowProvider();

    QImage requestImage(
        const QString &id,
        QSize *size,
        const QSize &requestedSize
    ) override;
};

} // namespace yobro::spike
