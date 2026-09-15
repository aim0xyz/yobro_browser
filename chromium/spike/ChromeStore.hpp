#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace yobro::spike {

/// Installing an extension straight from the Chrome Web Store.
///
/// The store has no install API, so the same route as the WebKit build is used:
/// ask Google's update service for the package of one extension id, then unpack
/// it locally. Everything about the package is treated as hostile input.
class ChromeStore {
public:
    /// The largest package that will be accepted, matching the WebKit build.
    static constexpr qint64 maximumPackageBytes = 100 * 1024 * 1024;

    /// The extension id in a store link, or the id itself. Returns nothing when
    /// the input is neither, so a random URL cannot start a download.
    [[nodiscard]] static std::optional<QString> identifier(const QString &input);

    /// The update service URL for one extension and one Chrome version.
    [[nodiscard]] static QUrl downloadUrl(const QString &id, const QString &chromeVersion);

    /// True when a redirect target may still be followed. Only Google's own
    /// download hosts are allowed, and only over HTTPS.
    [[nodiscard]] static bool isAllowedRedirect(const QUrl &url);

    /// The ZIP inside a CRX. Returns nothing for anything that is not a CRX 2 or
    /// CRX 3 file with a ZIP payload; a wrong format is never passed through.
    [[nodiscard]] static std::optional<QByteArray> zipPayload(const QByteArray &crx);

    struct Package {
        /// The directory the extension was unpacked into. Empty on failure.
        QString directory;
        QString name;
        QString version;
        /// The manifest's `manifest_version`, 0 when unreadable.
        int manifestVersion = 0;
        /// Permissions the extension asks for, sorted.
        QStringList permissions;
        /// Host patterns the extension asks for, sorted.
        QStringList hosts;
        /// Things the user should know before installing.
        QStringList warnings;
        /// Empty when the package can be installed.
        QString problem;
    };

    /// Turns downloaded bytes into an unpacked extension below `parentDirectory`.
    /// Refuses oversized packages, archives that would write outside the target,
    /// archives containing symbolic links, and anything without a usable
    /// `manifest.json`.
    [[nodiscard]] static Package acceptPackage(
        const QByteArray &crxOrZip,
        const QString &parentDirectory
    );

    /// Downloads the package for an extension id. Blocking; meant to be called
    /// from the UI thread while a progress note is shown.
    [[nodiscard]] static QByteArray download(const QString &id, QString &problem);
};

} // namespace yobro::spike
