#pragma once

// Shared front matter for the parts of SpikeWindow.
//
// The window used to be one 6200-line translation unit, which made every change
// a search through unrelated code and every edit a risk to the rest. Its methods
// now live in several files that all belong to the same class, so no member had
// to be made public and no behaviour had to move: only the definitions did.
// Each part includes this header, which carries the includes the window needs
// and the few helpers that were file-local before the split.

#include "spike/SpikeWindow.hpp"

#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/AdBlockRules.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/CertificateTrust.hpp"
#include "spike/Localization.hpp"
#include "spike/Theme.hpp"

#include <QAbstractItemModel>
#include <QAction>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyleHints>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEngineExtensionInfo>
#include <QWebEngineExtensionManager>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace yobro::spike::windowSupport {

/// Every page in this shell comes from the Qt engine. A different kind means the
/// engine boundary was bypassed, which is a programming error, not a page state.
[[nodiscard]] qtwebengine::QtBrowserPage *asQtPage(engine::BrowserPage *page);

/// The wording a permission prompt uses for what the site is asking for.
[[nodiscard]] QString permissionDevice(engine::WebPermission permission);
/// True for camera and microphone, which get the media wording and title.
[[nodiscard]] bool isMediaPermission(engine::WebPermission permission);
/// An extra sentence for permissions whose consequence is easy to miss.
[[nodiscard]] QString permissionCaution(engine::WebPermission permission);
/// The German download states, with the same wording as the WebKit build.
[[nodiscard]] QString downloadStateLabel(std::string_view state);
/// Shows a finished download in the platform's file manager.
[[nodiscard]] bool revealFileInPlatformShell(const QString &path);
/// A favicon as base64 PNG for the session file, or empty when it is too big.
[[nodiscard]] QString encodeIcon(const QIcon &icon);
/// The reverse of encodeIcon; an empty icon for anything unreadable.
[[nodiscard]] QIcon decodeIcon(const QString &encoded);
/// The start page markup, which differs between normal and private tabs.
[[nodiscard]] QString diagnosticStartPage(bool privatePage);

} // namespace yobro::spike::windowSupport
