#include "spike/SpikeWindowInternal.hpp"

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

QString diagnosticStartPage(bool privatePage) {
    if (privatePage) {
        return QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8"><title>Privates Browsen</title>
<style>body{font:16px system-ui;background:#151b17;color:#e5eadf;display:grid;place-items:center;height:100vh;margin:0}main{max-width:520px;padding:48px;border:1px solid #344037;border-radius:22px;background:#1d251f;box-shadow:0 18px 60px #0008}h1{font:500 38px Georgia,serif;margin:0 0 15px}p{line-height:1.65;color:#aeb9ac}</style></head><body><main><h1>Privates Browsen</h1><p>Cookies und Websitedaten bleiben nur bis zum Schließen dieses Tabs. Verlauf und Sitzung werden nicht gespeichert.</p></main></body></html>
)HTML");
    }
    return QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8"><title>YOBRO</title>
<style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;min-height:100vh;background:#151b17;color:#edf0e8;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;display:grid;place-items:center;overflow:hidden}main{text-align:center;width:min(900px,88vw);padding:48px 0 38px}.orbit{width:160px;height:160px;margin:0 auto 34px;border:2px solid #29342d;border-radius:50%;position:relative}.orbit:before,.orbit:after{content:"";position:absolute;border:2px solid #35433a;border-radius:50%;inset:20px}.orbit:after{inset:47px;border-color:#bf6c43;transform:rotate(-37deg) scaleX(1.48)}.planet{position:absolute;inset:57px;border:9px solid #bf6c43;border-radius:44% 56% 49% 51%;transform:rotate(-37deg)}.eyebrow{color:#b8c9b1;font:700 12px ui-monospace,SFMono-Regular,monospace;letter-spacing:.44em;margin-bottom:26px}.headline{font:500 clamp(55px,8vw,103px)/.98 Georgia,serif;letter-spacing:-.06em;margin:0}.detail{color:#9aa49b;font-size:16px;margin:28px 0 38px}.search{width:min(590px,100%);height:72px;margin:auto;display:flex;align-items:center;gap:13px;padding:0 16px 0 21px;border:1px solid #4a574d;border-radius:24px;background:#364038;color:#dfe5dc;font-size:18px;font-weight:600;text-align:left}.search b{color:#9aab98;font-size:28px;font-weight:300}.go{margin-left:auto;width:48px;height:48px;border-radius:15px;background:#84927e;color:#344037;display:grid;place-items:center;font-size:28px}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;width:min(810px,100%);margin:34px auto 0;text-align:left}.card{min-height:158px;padding:22px 24px;border:1px solid #344037;border-radius:20px;background:#273129}.icon{width:48px;height:48px;display:grid;place-items:center;border-radius:14px;background:#344b39;color:#e8eee3;font-size:25px}.card:nth-child(2) .icon{background:#443a56}.card:nth-child(3) .icon{background:#554132}.card h2{margin:21px 0 7px;font-size:19px}.card p{margin:0;color:#98a398;font-size:13px}@media(max-width:650px){.cards{grid-template-columns:1fr}.headline{font-size:53px}.orbit{transform:scale(.78);margin-bottom:10px}}
.search{padding:0}.search input{flex:1;height:100%;border:0;background:transparent;color:#dfe5dc;font-size:18px;font-weight:600;outline:0;padding:0}.search input::placeholder{color:#9aab98;font-weight:600}.search button{margin-left:auto;width:48px;height:48px;border:0;border-radius:15px;background:#84927e;color:#344037;font-size:28px;cursor:pointer}.search form{display:flex;align-items:center;gap:13px;width:100%;height:100%;padding:0 16px 0 21px}.card{text-decoration:none;color:inherit;display:block}.card:hover{border-color:#4a574d}
</style></head><body><main><div class="orbit"><div class="planet"></div></div><div class="eyebrow">DEIN RAUM IM WEB</div><h1 class="headline">Weniger Suchen.<br>Mehr Entdecken.</h1><p class="detail">Für große Ideen und die kleinen Umwege dazwischen.</p><div class="search"><form action="https://duckduckgo.com/" method="get"><b>⌕</b><input name="q" type="search" autofocus placeholder="Wohin zieht es dich?" aria-label="Suchen oder Adresse eingeben"><button type="submit" aria-label="Suche starten">→</button></form></div><div class="cards"><a class="card" href="https://wikipedia.org"><div class="icon">◉</div><h2>Entdecken</h2><p>wikipedia.org</p></a><a class="card" href="https://github.com"><div class="icon">{ }</div><h2>Entwickeln</h2><p>github.com</p></a><a class="card" href="https://news.ycombinator.com"><div class="icon">▤</div><h2>Lesen</h2><p>news.ycombinator.com</p></a></div></main></body></html>
)HTML");
}

} // namespace yobro::spike::windowSupport
