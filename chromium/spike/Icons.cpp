#include "spike/Icons.hpp"

#include "spike/Theme.hpp"

#include <QAbstractButton>
#include <QFile>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QSvgRenderer>

namespace yobro::spike {
namespace {

struct InstalledIcon {
    QPointer<QAbstractButton> button;
    QString name;
    IconRole role;
};

QList<InstalledIcon> &installedIcons() {
    static QList<InstalledIcon> icons;
    return icons;
}

QColor roleColor(IconRole role, bool dark) {
    const ThemePalette &palette = themePalette(dark);
    switch (role) {
    case IconRole::normal: return QColor(palette.ink);
    case IconRole::muted: return QColor(palette.textMuted);
    case IconRole::accent: return QColor(palette.moss);
    case IconRole::brand: return QColor(palette.brandOrange);
    }
    return QColor(palette.ink);
}

/// Lucide draws with stroke="currentColor"; the tint is substituted before
/// rendering so one file serves both appearances.
QIcon renderIcon(const QString &name, IconRole role, bool dark) {
    QFile file(QStringLiteral(":/yobro/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return QIcon();
    QString svg = QString::fromUtf8(file.readAll());
    svg.replace(QStringLiteral("currentColor"), roleColor(role, dark).name(QColor::HexRgb));
    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) return QIcon();
    // Lucide's 24px view box with a 2px stroke stays legible when rendered
    // slightly into the padding, so small chrome buttons get crisp glyphs.
    QIcon icon;
    for (const int extent : {16, 20, 24, 32}) {
        QPixmap pixmap(extent * 2, extent * 2);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(2);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(QPointF(2, 2), QSizeF(extent - 4, extent - 4)));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

void installIcon(QAbstractButton *button, const QString &name, IconRole role) {
    if (!button) return;
    button->setIcon(renderIcon(name, role, currentAppearanceIsDark()));
    installedIcons().append({button, name, role});
}

void retintIcons(bool dark) {
    auto &icons = installedIcons();
    icons.removeIf([](const InstalledIcon &entry) { return entry.button.isNull(); });
    for (const InstalledIcon &entry : icons)
        entry.button->setIcon(renderIcon(entry.name, entry.role, dark));
}

} // namespace yobro::spike
