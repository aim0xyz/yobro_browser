#pragma once

#include <QString>

namespace yobro::spike {

/// The colours of the browser chrome for one appearance.
struct ThemePalette {
    QString shell;
    QString sidebarTop;
    QString sidebarBottom;
    QString surface;
    QString surfaceRaised;
    QString surfaceHover;
    QString border;
    QString borderStrong;
    QString text;
    QString textMuted;
    QString accent;
    QString selection;
    /// Sheets and popups follow the same appearance as the window.
    QString sheetBackground;
    QString sheetSurface;
    QString sheetHover;
    QString sheetBorder;
    QString sheetText;
    QString sheetMuted;
    QString sheetSelection;
};

/// The window follows the system appearance, like the WebKit build does.
[[nodiscard]] const ThemePalette &themePalette(bool dark);

/// Style sheet for the main window and its sidebar.
[[nodiscard]] QString windowStyleSheet(bool dark);

/// Shared style sheet for dialogs and popups.
[[nodiscard]] QString sheetStyleSheet(bool dark);

} // namespace yobro::spike
