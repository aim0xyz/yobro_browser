#include "spike/SpikeWindowInternal.hpp"

#include <QWidget>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace yobro::spike::windowSupport {

qtwebengine::QtBrowserPage *asQtPage(engine::BrowserPage *page) {
    auto *qtPage = dynamic_cast<qtwebengine::QtBrowserPage *>(page);
    if (!qtPage)
        throw std::runtime_error("The Qt engine returned a non-Qt page to the feasibility shell.");
    return qtPage;
}

QString permissionDevice(engine::WebPermission permission) {
    switch (permission) {
    case engine::WebPermission::camera:
        return L(QStringLiteral("die Kamera"));
    case engine::WebPermission::microphone:
        return L(QStringLiteral("das Mikrofon"));
    case engine::WebPermission::microphoneAndCamera:
        return L(QStringLiteral("Kamera und Mikrofon"));
    case engine::WebPermission::geolocation:
        return L(QStringLiteral("deinen Standort"), QStringLiteral("your location"));
    case engine::WebPermission::notifications:
        return L(QStringLiteral("Mitteilungen"), QStringLiteral("notifications"));
    case engine::WebPermission::clipboard:
        return L(QStringLiteral("die Zwischenablage"), QStringLiteral("the clipboard"));
    case engine::WebPermission::localFonts:
        return L(QStringLiteral("deine installierten Schriften"), QStringLiteral("your installed fonts"));
    case engine::WebPermission::pointerLock:
        return L(QStringLiteral("den Mauszeiger"), QStringLiteral("the mouse pointer"));
    case engine::WebPermission::screenShare:
        return L(QStringLiteral("deinen Bildschirm"), QStringLiteral("your screen"));
    case engine::WebPermission::screenShareWithAudio:
        return L(QStringLiteral("deinen Bildschirm und dessen Ton"), QStringLiteral("your screen and its audio"));
    }
    return L(QStringLiteral("Mediengeräte"));
}

/// True for the two capture permissions that reach a device; the rest are site
/// capabilities and get a less alarming window title.
bool isMediaPermission(engine::WebPermission permission) {
    switch (permission) {
    case engine::WebPermission::camera:
    case engine::WebPermission::microphone:
    case engine::WebPermission::microphoneAndCamera:
        return true;
    case engine::WebPermission::geolocation:
    case engine::WebPermission::notifications:
    case engine::WebPermission::clipboard:
    case engine::WebPermission::localFonts:
    case engine::WebPermission::pointerLock:
        return false;
    case engine::WebPermission::screenShare:
    case engine::WebPermission::screenShareWithAudio:
        // Sharing a screen is a capture, and the most far-reaching one.
        return true;
    }
    return true;
}

/// Extra sentence for permissions whose consequences are not obvious from the
/// question alone. Empty means the shared visit-only note is enough.
QString permissionCaution(engine::WebPermission permission) {
    switch (permission) {
    case engine::WebPermission::clipboard:
        return L(
            QStringLiteral("Die Seite kann dann lesen, was du zuletzt kopiert hast, auch Passwörter."),
            QStringLiteral("The site can then read what you copied last, including passwords.")
        );
    case engine::WebPermission::localFonts:
        return L(
            QStringLiteral("Die Liste deiner Schriften macht deinen Rechner leichter wiedererkennbar."),
            QStringLiteral("The list of your fonts makes your machine easier to recognize again.")
        );
    case engine::WebPermission::camera:
    case engine::WebPermission::microphone:
    case engine::WebPermission::microphoneAndCamera:
    case engine::WebPermission::geolocation:
    case engine::WebPermission::notifications:
    case engine::WebPermission::pointerLock:
        return {};
    case engine::WebPermission::screenShare:
    case engine::WebPermission::screenShareWithAudio:
        return L(
            QStringLiteral("Alles, was auf der geteilten Fläche zu sehen ist, geht an die Website — auch Fenster, die du später davor legst."),
            QStringLiteral("Everything visible on the shared surface goes to the website, including windows you put in front of it later.")
        );
    }
    return {};
}

QString downloadStateLabel(std::string_view state) {
    if (state == "completed") return L(QStringLiteral("Abgeschlossen"));
    if (state == "cancelled") return L(QStringLiteral("Abgebrochen"));
    if (state == "failed") return L(QStringLiteral("Fehlgeschlagen"));
    if (state == "interrupted") return L(QStringLiteral("Durch Neustart unterbrochen"));
    return L(QStringLiteral("Wird geladen"));
}

bool revealFileInPlatformShell(const QString &path) {
#ifdef Q_OS_MACOS
    return QProcess::startDetached(
        QStringLiteral("/usr/bin/open"),
        {QStringLiteral("-R"), path}
    );
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
#endif
}

/// Favicons are stored as a small PNG so restored tabs show their icon before
/// the page has loaded again.
QString encodeIcon(const QIcon &icon) {
    if (icon.isNull()) return {};
    const QPixmap pixmap = icon.pixmap(32, 32);
    if (pixmap.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) return {};
    if (!pixmap.save(&buffer, "PNG")) return {};
    // Keeps the session file small; a 32 px icon stays far below this.
    if (bytes.size() > 32 * 1024) return {};
    return QString::fromLatin1(bytes.toBase64());
}

QIcon decodeIcon(const QString &encoded) {
    if (encoded.isEmpty() || encoded.size() > 64 * 1024) return {};
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (bytes.isEmpty()) return {};
    QPixmap pixmap;
    if (!pixmap.loadFromData(bytes, "PNG")) return {};
    return QIcon(pixmap);
}

void configureMacWindowFrame(QWidget *window) {
#if defined(__APPLE__)
    if (!window || window->testAttribute(Qt::WA_DontShowOnScreen)) return;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return;
    if (!window->windowHandle()) return;
    id nsView = reinterpret_cast<id>(window->winId());
    if (!nsView) return;
    id nsWindow = reinterpret_cast<id (*)(id, SEL)>(&objc_msgSend)(nsView, sel_registerName("window"));
    if (!nsWindow) return;

    reinterpret_cast<void (*)(id, SEL, bool)>(&objc_msgSend)(
        nsWindow, sel_registerName("setTitlebarAppearsTransparent:"), true
    );
    reinterpret_cast<void (*)(id, SEL, long)>(&objc_msgSend)(
        nsWindow, sel_registerName("setTitleVisibility:"), 1
    );
    long mask = reinterpret_cast<long (*)(id, SEL)>(&objc_msgSend)(
        nsWindow, sel_registerName("styleMask")
    );
    mask |= (1L << 15); // NSWindowStyleMaskFullSizeContentView
    reinterpret_cast<void (*)(id, SEL, long)>(&objc_msgSend)(
        nsWindow, sel_registerName("setStyleMask:"), mask
    );
    reinterpret_cast<void (*)(id, SEL, bool)>(&objc_msgSend)(
        nsWindow, sel_registerName("setMovableByWindowBackground:"), true
    );
#else
    Q_UNUSED(window);
#endif
}

QString diagnosticStartPage(bool privatePage) {
    const bool dark = currentAppearanceIsDark();
    const ThemePalette &palette = themePalette(dark);
    const QColor mossColor(palette.moss);
    const QString mossGlow = QStringLiteral("rgba(%1, %2, %3, 0.08)").arg(
        QString::number(mossColor.red()),
        QString::number(mossColor.green()),
        QString::number(mossColor.blue())
    );

    if (privatePage) {
        const QString title = L(QStringLiteral("Privates Browsen"), QStringLiteral("Private browsing"));
        const QString detail = L(
            QStringLiteral("Cookies und Websitedaten bleiben nur bis zum Schließen des Tabs. Verlauf und Sitzung werden nicht gespeichert."),
            QStringLiteral("Cookies and website data last only until you close the tab. History and session are not saved.")
        );
        const QString searchPlaceholder = L(
            QStringLiteral("Privat suchen oder URL eingeben"),
            QStringLiteral("Search privately or enter URL")
        );
        const QString searchHint = L(
            QStringLiteral("Adresse eingeben oder direkt im Web suchen · Enter zum Öffnen"),
            QStringLiteral("Enter an address or search the web · Press Enter to open")
        );

        return QStringLiteral(R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>%1</title>
<style>
:root { color-scheme: %2; }
* { box-sizing: border-box; }
body {
    margin: 0;
    min-height: 100vh;
    background: radial-gradient(circle at 50% 35%, %3 0%, transparent 60%), %4;
    color: %5;
    font-family: -apple-system, BlinkMacSystemFont, "SF Pro Display", "SF Pro Text", "Segoe UI", sans-serif;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
}
main {
    text-align: center;
    width: min(540px, 92vw);
}
.icon-wrap {
    margin: 0 auto 16px;
    width: 64px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
    color: %6;
}
.icon-wrap svg {
    width: 54px;
    height: 54px;
    stroke: currentColor;
    stroke-width: 1.6;
    fill: none;
    stroke-linecap: round;
    stroke-linejoin: round;
}
h1 {
    font-family: -apple-system, BlinkMacSystemFont, "SF Pro Rounded", "SF Pro Display", sans-serif;
    font-size: 36px;
    font-weight: 600;
    letter-spacing: -0.04em;
    margin: 0 0 8px;
    color: %5;
}
p.detail {
    margin: 0 0 28px;
    font-size: 13px;
    line-height: 1.5;
    color: %7;
    max-width: 460px;
    margin-left: auto;
    margin-right: auto;
}
.card {
    background: %8;
    border: 1px solid %9;
    border-radius: 18px;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.08);
    padding: 16px;
    text-align: left;
}
.search-box {
    display: flex;
    align-items: center;
    gap: 10px;
    background: %10;
    border: 1px solid %9;
    border-radius: 12px;
    height: 48px;
    padding: 0 14px;
    transition: border-color 0.15s ease, box-shadow 0.15s ease;
}
.search-box:focus-within {
    border-color: %6;
    box-shadow: 0 0 0 1.5px %6;
}
.search-box svg {
    width: 18px;
    height: 18px;
    stroke: %6;
    stroke-width: 2;
    fill: none;
    flex-shrink: 0;
}
.search-box form {
    display: flex;
    align-items: center;
    width: 100%;
    height: 100%;
}
.search-box input {
    flex: 1;
    height: 100%;
    border: 0;
    background: transparent;
    color: %5;
    font-size: 15px;
    font-family: inherit;
    outline: 0;
    padding: 0;
}
.search-box input::placeholder {
    color: %7;
}
.search-box button {
    border: 0;
    background: transparent;
    color: %6;
    cursor: pointer;
    padding: 4px;
    display: flex;
    align-items: center;
    justify-content: center;
}
.search-box button svg {
    width: 16px;
    height: 16px;
}
.hint {
    margin: 10px 4px 0;
    font-size: 11px;
    color: %7;
}
</style>
</head>
<body>
<main>
    <div class="icon-wrap">
        <svg viewBox="0 0 24 24"><circle cx="6" cy="15" r="4"/><circle cx="18" cy="15" r="4"/><path d="M14 15a2 2 0 0 0-4 0"/><path d="M2.5 13 5 7c.7-1.3 1.4-2 3-2"/><path d="M21.5 13 19 7c-.7-1.3-1.5-2-3-2"/></svg>
    </div>
    <h1>%1</h1>
    <p class="detail">%11</p>
    <div class="card">
        <div class="search-box">
            <svg viewBox="0 0 24 24"><circle cx="6" cy="15" r="4"/><circle cx="18" cy="15" r="4"/><path d="M14 15a2 2 0 0 0-4 0"/><path d="M2.5 13 5 7c.7-1.3 1.4-2 3-2"/><path d="M21.5 13 19 7c-.7-1.3-1.5-2-3-2"/></svg>
            <form action="https://duckduckgo.com/" method="get">
                <input name="q" type="search" autofocus placeholder="%12" aria-label="%12">
                <button type="submit" aria-label="Search">
                    <svg viewBox="0 0 24 24"><polyline points="9 10 4 15 9 20"/><path d="M20 4v7a4 4 0 0 1-4 4H4"/></svg>
                </button>
            </form>
        </div>
        <div class="hint">%13</div>
    </div>
</main>
</body>
</html>)HTML")
            .arg(title)                               // %1
            .arg(dark ? QStringLiteral("dark") : QStringLiteral("light")) // %2
            .arg(mossGlow)                            // %3
            .arg(palette.paper)                       // %4
            .arg(palette.ink)                         // %5
            .arg(palette.moss)                        // %6
            .arg(palette.textMuted)                   // %7
            .arg(palette.surface)                     // %8
            .arg(palette.borderSoft)                  // %9
            .arg(palette.field)                       // %10
            .arg(detail)                              // %11
            .arg(searchPlaceholder)                   // %12
            .arg(searchHint);                         // %13
    }

    const QString title = QStringLiteral("YoBro");
    const QString subtitle = L(
        QStringLiteral("Wohin geht es als Nächstes?"),
        QStringLiteral("Where to next?")
    );
    const QString searchPlaceholder = L(
        QStringLiteral("Suchen oder URL eingeben"),
        QStringLiteral("Search or enter URL")
    );
    const QString searchHint = L(
        QStringLiteral("Adresse eingeben oder direkt im Web suchen · Enter zum Öffnen"),
        QStringLiteral("Enter an address or search the web · Press Enter to open")
    );
    const QString discoverTitle = L(QStringLiteral("Entdecken"), QStringLiteral("Discover"));
    const QString developTitle = L(QStringLiteral("Entwickeln"), QStringLiteral("Develop"));
    const QString readTitle = L(QStringLiteral("Lesen"), QStringLiteral("Read"));

    return QStringLiteral(R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>%1</title>
<style>
:root { color-scheme: %2; }
* { box-sizing: border-box; }
body {
    margin: 0;
    min-height: 100vh;
    background: radial-gradient(circle at 50% 35%, %3 0%, transparent 65%), %4;
    color: %5;
    font-family: -apple-system, BlinkMacSystemFont, "SF Pro Display", "SF Pro Text", "Segoe UI", sans-serif;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
}
main {
    text-align: center;
    width: min(540px, 92vw);
}
.brand-mark {
    margin: 0 auto 14px;
    width: 64px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
}
.brand-mark svg {
    width: 58px;
    height: 58px;
}
h1 {
    font-family: -apple-system, BlinkMacSystemFont, "SF Pro Rounded", "SF Pro Display", sans-serif;
    font-size: 42px;
    font-weight: 600;
    letter-spacing: -0.05em;
    margin: 0 0 6px;
    color: %5;
}
p.subtitle {
    margin: 0 0 26px;
    font-size: 13px;
    color: %6;
}
.card {
    background: %7;
    border: 1px solid %8;
    border-radius: 18px;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.08);
    padding: 16px;
    text-align: left;
}
.search-box {
    display: flex;
    align-items: center;
    gap: 10px;
    background: %9;
    border: 1px solid %8;
    border-radius: 12px;
    height: 48px;
    padding: 0 14px;
    transition: border-color 0.15s ease, box-shadow 0.15s ease;
}
.search-box:focus-within {
    border-color: %10;
    box-shadow: 0 0 0 1.5px %10;
}
.search-box svg {
    width: 18px;
    height: 18px;
    stroke: %10;
    stroke-width: 2;
    fill: none;
    flex-shrink: 0;
}
.search-box form {
    display: flex;
    align-items: center;
    width: 100%;
    height: 100%;
}
.search-box input {
    flex: 1;
    height: 100%;
    border: 0;
    background: transparent;
    color: %5;
    font-size: 15px;
    font-family: inherit;
    outline: 0;
    padding: 0;
}
.search-box input::placeholder {
    color: %6;
}
.search-box button {
    border: 0;
    background: transparent;
    color: %10;
    cursor: pointer;
    padding: 4px;
    display: flex;
    align-items: center;
    justify-content: center;
}
.search-box button svg {
    width: 16px;
    height: 16px;
}
.hint {
    margin: 10px 4px 0;
    font-size: 11px;
    color: %6;
}
.shortcuts {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 12px;
    margin-top: 20px;
}
.shortcut {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    background: %7;
    border: 1px solid %8;
    border-radius: 14px;
    padding: 14px 10px;
    text-decoration: none;
    color: inherit;
    transition: transform 0.15s ease, border-color 0.15s ease, background 0.15s ease;
}
.shortcut:hover {
    transform: translateY(-2px);
    border-color: %10;
    background: %9;
}
.shortcut-icon {
    width: 32px;
    height: 32px;
    border-radius: 9px;
    background: %11;
    color: %10;
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 8px;
}
.shortcut-icon svg {
    width: 18px;
    height: 18px;
    stroke: currentColor;
    stroke-width: 2;
    fill: none;
}
.shortcut-title {
    font-size: 13px;
    font-weight: 600;
    color: %5;
    margin-bottom: 2px;
}
.shortcut-domain {
    font-size: 11px;
    color: %6;
}
</style>
</head>
<body>
<main>
    <div class="brand-mark">
        <svg viewBox="0 0 64 64" fill="none">
            <rect width="64" height="64" rx="16" fill="%12"/>
            <path d="M22 20L32 35L42 20M32 35V45" stroke="#FFFFFF" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
    </div>
    <h1>%1</h1>
    <p class="subtitle">%13</p>
    <div class="card">
        <div class="search-box">
            <svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
            <form action="https://duckduckgo.com/" method="get">
                <input name="q" type="search" autofocus placeholder="%14" aria-label="%14">
                <button type="submit" aria-label="Search">
                    <svg viewBox="0 0 24 24"><polyline points="9 10 4 15 9 20"/><path d="M20 4v7a4 4 0 0 1-4 4H4"/></svg>
                </button>
            </form>
        </div>
        <div class="hint">%15</div>
    </div>
    <div class="shortcuts">
        <a class="shortcut" href="https://wikipedia.org">
            <div class="shortcut-icon">
                <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>
            </div>
            <div class="shortcut-title">%16</div>
            <div class="shortcut-domain">wikipedia.org</div>
        </a>
        <a class="shortcut" href="https://github.com">
            <div class="shortcut-icon">
                <svg viewBox="0 0 24 24"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></svg>
            </div>
            <div class="shortcut-title">%17</div>
            <div class="shortcut-domain">github.com</div>
        </a>
        <a class="shortcut" href="https://news.ycombinator.com">
            <div class="shortcut-icon">
                <svg viewBox="0 0 24 24"><line x1="4" y1="9" x2="20" y2="9"/><line x1="4" y1="15" x2="20" y2="15"/><line x1="10" y1="3" x2="8" y2="21"/><line x1="16" y1="3" x2="14" y2="21"/></svg>
            </div>
            <div class="shortcut-title">%18</div>
            <div class="shortcut-domain">news.ycombinator.com</div>
        </a>
    </div>
</main>
</body>
</html>)HTML")
        .arg(title)                                   // %1
        .arg(dark ? QStringLiteral("dark") : QStringLiteral("light")) // %2
        .arg(mossGlow)                                // %3
        .arg(palette.paper)                           // %4
        .arg(palette.ink)                             // %5
        .arg(palette.textMuted)                       // %6
        .arg(palette.surface)                         // %7
        .arg(palette.borderSoft)                      // %8
        .arg(palette.field)                           // %9
        .arg(palette.moss)                            // %10
        .arg(palette.selectionWash)                   // %11
        .arg(palette.brandOrange)                     // %12
        .arg(subtitle)                                // %13
        .arg(searchPlaceholder)                       // %14
        .arg(searchHint)                              // %15
        .arg(discoverTitle)                           // %16
        .arg(developTitle)                            // %17
        .arg(readTitle);                              // %18
}

} // namespace yobro::spike::windowSupport
