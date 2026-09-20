#include "spike/qml/QmlTheme.hpp"

#include "spike/Localization.hpp"
#include "spike/Theme.hpp"

#include <QGuiApplication>
#include <QStyleHints>

namespace yobro::spike {
namespace {

QColor tokenColor(const QString &token) {
    return QColor(token);
}

} // namespace

QmlTheme::QmlTheme(QObject *parent) : QObject(parent) {
    watchAppearance();
}

void QmlTheme::watchAppearance() {
    if (QGuiApplication::instance()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, &QmlTheme::paletteChanged);
    }
}

bool QmlTheme::dark() const {
    return currentAppearanceIsDark();
}

QColor QmlTheme::ink() const { return tokenColor(themePalette(dark()).ink); }
QColor QmlTheme::moss() const { return tokenColor(themePalette(dark()).moss); }
QColor QmlTheme::paper() const { return tokenColor(themePalette(dark()).paper); }
QColor QmlTheme::surface() const { return tokenColor(themePalette(dark()).surface); }
QColor QmlTheme::chromeTop() const { return tokenColor(themePalette(dark()).chromeTop); }
QColor QmlTheme::chromeBottom() const { return tokenColor(themePalette(dark()).chromeBottom); }
QColor QmlTheme::sage() const { return tokenColor(themePalette(dark()).sage); }
QColor QmlTheme::lilac() const { return tokenColor(themePalette(dark()).lilac); }
QColor QmlTheme::peach() const { return tokenColor(themePalette(dark()).peach); }
QColor QmlTheme::field() const { return tokenColor(themePalette(dark()).field); }
QColor QmlTheme::border() const { return tokenColor(themePalette(dark()).border); }
QColor QmlTheme::brandOrange() const { return tokenColor(themePalette(dark()).brandOrange); }
QColor QmlTheme::textMuted() const { return tokenColor(themePalette(dark()).textMuted); }
QColor QmlTheme::hoverWash() const { return tokenColor(themePalette(dark()).hoverWash); }
QColor QmlTheme::pressedWash() const { return tokenColor(themePalette(dark()).pressedWash); }
QColor QmlTheme::selectionWash() const { return tokenColor(themePalette(dark()).selectionWash); }
QColor QmlTheme::mossTintWash() const { return tokenColor(themePalette(dark()).mossTintWash); }
QColor QmlTheme::borderHairline() const { return tokenColor(themePalette(dark()).borderHairline); }
QColor QmlTheme::borderSoft() const { return tokenColor(themePalette(dark()).borderSoft); }
QColor QmlTheme::borderField() const { return tokenColor(themePalette(dark()).borderField); }

QString QmlTheme::fontRounded() {
    // macOS hides the SF families from non-Apple processes: an installed
    // "SF Pro Rounded" would be honoured, otherwise Qt falls back to the
    // hidden system family (San Francisco), exactly like the widget shell's
    // style sheet chain.
    return QStringLiteral("SF Pro Rounded");
}

QString QmlTheme::fontText() {
    return QStringLiteral(".AppleSystemUIFont");
}

QString QmlTheme::fontMono() {
    return QStringLiteral("Menlo");
}

QUrl QmlTheme::iconUrl(const QString &name, const QString &role) const {
    return QUrl(QStringLiteral("image://yobroicon/%1?role=%2&dark=%3")
                    .arg(name, role, dark() ? QStringLiteral("1") : QStringLiteral("0")));
}

QString QmlTheme::loc(const QString &german, const QString &english) const {
    return L(german, english);
}

} // namespace yobro::spike
