#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <vector>

namespace yobro::spike {

/// One browser profile found on this Mac.
struct ImportProfileEntry {
    QString id;
    QString browser;
    QString name;
    QString path;
};

/// A bookmark, history entry or open tab from another browser.
struct ImportedLink {
    QString title;
    QString url;
    QString folder;
    /// Unix seconds; 0 when the source has no timestamp.
    double timestamp = 0;
    long long visits = 1;
    bool pinned = false;
};

struct ImportedCookie {
    QString name;
    QString value;
    QString domain;
    QString path;
    /// Unix seconds; 0 means a cookie that only lives for the session.
    double expires = 0;
    bool secure = false;
    bool httpOnly = false;
    QString sameSite;
};

struct ImportedBrowserData {
    std::vector<ImportedLink> bookmarks;
    std::vector<ImportedLink> history;
    std::vector<ImportedLink> tabs;
    std::vector<ImportedCookie> cookies;
    QStringList warnings;
    /// Per data kind, what the source can offer. The wording comes from the
    /// shared helper script, so both engines explain it identically.
    QMap<QString, QString> availability;
    /// Empty unless the whole read failed.
    QString problem;

    [[nodiscard]] std::size_t total() const {
        return bookmarks.size() + history.size() + tabs.size() + cookies.size();
    }
};

/// Reads bookmarks, history, open tabs and cookies out of another browser.
///
/// All of the reading happens in the helper script that is shared with the
/// WebKit build, so both engines understand the same profiles and export files
/// and apply the same URL filtering. The source is only ever read.
class BrowserDataImport {
public:
    /// The data kinds this can transfer, using the script's own names. Passwords
    /// are handled by `NativePasswordImport` because they need the Keychain.
    [[nodiscard]] static QStringList kinds();
    /// A readable label for a kind, matching the WebKit build's wording.
    [[nodiscard]] static QString kindLabel(const QString &kind);

    /// The browser profiles found under `home`, or under the real home when
    /// empty. Safari is always listed because its data does not live in a
    /// profile directory.
    [[nodiscard]] static std::vector<ImportProfileEntry> profiles(
        const QString &home,
        QString &problem
    );

    /// Reads the requested kinds from a live profile.
    [[nodiscard]] static ImportedBrowserData readProfile(
        const QString &browser,
        const QString &profilePath,
        const QStringList &kinds
    );

    /// Reads one kind from an export file, for what a live profile cannot serve,
    /// such as encrypted Chromium cookies.
    [[nodiscard]] static ImportedBrowserData readFile(const QString &kind, const QString &filePath);
};

} // namespace yobro::spike
