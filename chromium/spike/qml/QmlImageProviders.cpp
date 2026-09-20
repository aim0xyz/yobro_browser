#include "spike/qml/QmlImageProviders.hpp"

#include "spike/Theme.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

#include <algorithm>

namespace yobro::spike {
namespace {

QColor roleColor(const QString &role, bool dark) {
    const ThemePalette &palette = themePalette(dark);
    if (role == QLatin1String("muted")) return QColor(palette.textMuted);
    if (role == QLatin1String("accent")) return QColor(palette.moss);
    if (role == QLatin1String("brand")) return QColor(palette.brandOrange);
    return QColor(palette.ink);
}

qreal screenScale() {
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        return screen->devicePixelRatio();
    return 1.0;
}

/// Lucide draws with stroke="currentColor"; the tint is substituted before
/// rendering so one file serves both appearances. The two-point inset keeps
/// the 24px view box's stroke legible at small chrome sizes, exactly like the
/// widget shell's renderer.
QImage renderIconFile(const QString &name, const QColor &tint, const QSize &logicalSize, qreal scale) {
    QFile file(QStringLiteral(":/yobro/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QString svg = QString::fromUtf8(file.readAll());
    svg.replace(QStringLiteral("currentColor"), tint.name(QColor::HexRgb));
    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) return {};

    const QSize deviceSize = logicalSize * scale;
    QImage image(deviceSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(scale);
    // QPainter honours the image's device pixel ratio, so the rect below is
    // in logical pixels while the buffer stays at Retina resolution.
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(
        &painter,
        QRectF(QPointF(2.0, 2.0),
               QSizeF(logicalSize.width() - 4.0, logicalSize.height() - 4.0))
    );
    painter.end();
    return image;
}

QStringList urlPairs(const QString &id, QString *path) {
    const int queryStart = id.indexOf(QLatin1Char('?'));
    if (queryStart < 0) {
        *path = id;
        return {};
    }
    *path = id.left(queryStart);
    return id.mid(queryStart + 1).split(QLatin1Char('&'));
}

QString pairValue(const QStringList &pairs, const QString &key) {
    for (const QString &pair : pairs) {
        const int equals = pair.indexOf(QLatin1Char('='));
        if (equals > 0 && pair.left(equals) == key)
            return pair.mid(equals + 1);
    }
    return {};
}

} // namespace

QmlIconProvider::QmlIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage QmlIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    QString name;
    const QStringList pairs = urlPairs(id, &name);
    const QString role = pairValue(pairs, QStringLiteral("role"));
    const bool dark = pairValue(pairs, QStringLiteral("dark")) == QLatin1String("1");

    QSize logical = requestedSize;
    if (logical.isEmpty()) logical = QSize(24, 24);
    logical = QSize(std::clamp(logical.width(), 8, 96), std::clamp(logical.height(), 8, 96));

    QImage image = renderIconFile(name, roleColor(role.isEmpty() ? QStringLiteral("ink") : role, dark),
                                  logical, screenScale());
    if (size) *size = logical;
    return image;
}

QmlGlowProvider::QmlGlowProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage QmlGlowProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    // id: "<width>x<height>?dark=<0|1>" — logical pixels, drawn at the window
    // scale so the falloff stays smooth on Retina.
    QString sizePart;
    const QStringList pairs = urlPairs(id, &sizePart);
    const bool dark = pairValue(pairs, QStringLiteral("dark")) == QLatin1String("1");

    QSize logical = requestedSize;
    const QStringList dimensions = sizePart.split(QLatin1Char('x'));
    if (logical.isEmpty() && dimensions.size() == 2)
        logical = QSize(dimensions.first().toInt(), dimensions.last().toInt());
    if (logical.isEmpty()) logical = QSize(420, 420);
    logical = logical.boundedTo(QSize(4096, 4096)).expandedTo(QSize(1, 1));

    const QColor moss(themePalette(dark).moss);
    // The WebKit start page draws RadialGradient(moss 8%, clear) with radius
    // 20 → 420; the 20px solid core is imperceptible, the radius is what the
    // eye reads. QPainter on a QImage works in device pixels, so the radius
    // scales with the image.
    const qreal scale = screenScale();
    const QSize deviceSize = logical * scale;
    // Logical coordinates again: the painter scales by the image DPR.
    QRadialGradient gradient(QPointF(logical.width() / 2.0, logical.height() / 2.0), 420.0);
    gradient.setColorAt(0.0, QColor(moss.red(), moss.green(), moss.blue(), qRound(255 * 0.08)));
    gradient.setColorAt(1.0, QColor(moss.red(), moss.green(), moss.blue(), 0));

    QImage image(deviceSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    QPainter painter(&image);
    painter.fillRect(QRect(QPoint(0, 0), logical), gradient);
    painter.end();
    if (size) *size = logical;
    return image;
}

} // namespace yobro::spike
