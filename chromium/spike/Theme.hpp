#pragma once

#include <QString>

namespace yobro::spike {

/// The colours of the browser chrome for one appearance.
///
/// Every source token descends from the WebKit build's `Theme.swift`. The
/// `chromium-theme-tokens` test parses both files and fails when they drift
/// apart, so the two builds keep one visual identity. Derived washes are the
/// opacity blends the WebKit views build inline.
struct ThemePalette {
    // Source tokens (Theme.swift order).
    QString ink;          // Theme.text — text and hairline base
    QString moss;         // Theme.accent — the sage-green accent
    QString paper;        // Theme.page — content background
    QString surface;      // Theme.surface — cards and raised fields
    QString chromeTop;    // Theme.chromeTop — window gradient, top
    QString chromeBottom; // Theme.chromeBottom — window gradient, bottom
    QString sage;         // Theme.sage
    QString lilac;        // Theme.lilac
    QString peach;        // Theme.peach
    QString folderSage;   // Theme.folderSage
    QString folderLilac;  // Theme.folderLilac
    QString folderPeach;  // Theme.folderPeach
    QString folderSky;    // Theme.folderSky
    QString folderSand;   // Theme.folderSand
    QString folderRose;   // Theme.folderRose
    QString field;        // Theme.field — text field fill
    QString border;       // Theme.border
    QString brandOrange;  // Theme.brandOrange — agent and brand moments

    // Derived blends (the WebKit views write these as .opacity()).
    QString textMuted;       // ink 55% over paper
    QString hoverWash;       // ink 6.5% over paper — the YOBROButtonStyle hover
    QString pressedWash;     // ink 12% over paper
    QString selectionWash;   // moss 14% over paper
    QString mossTintWash;    // moss 10% over paper — popover row hover
    QString borderHairline;  // rgba(ink, 0.08) — content card hairline
    QString borderSoft;      // rgba(border, 0.48) — quiet control outlines
    QString borderField;     // rgba(border, 0.65) — text field outlines

    // Backwards-compatible aliases consumed by the style sheets.
    QString shell;           // paper
    QString sidebarTop;      // chromeTop
    QString sidebarBottom;   // chromeBottom
    QString text;            // ink
    QString surfaceRaised;   // field
    QString surfaceHover;    // hoverWash
    QString surfacePressed;  // pressedWash
    QString borderStrong;    // border
    QString accent;          // moss
    QString selection;       // selectionWash
    QString sheetBackground; // paper
    QString sheetSurface;    // surface
    QString sheetHover;      // mossTintWash
    QString sheetBorder;     // borderField
    QString sheetText;       // ink
    QString sheetMuted;      // textMuted
    QString sheetSelection;  // mossTintWash
};

/// The window follows the system appearance, like the WebKit build does.
/// `YOBRO_TEST_APPEARANCE=light|dark` pins it for golden-reference captures.
/// The GUI layer installs the system probe (`setSystemDarkProbe`) because this
/// header is also compiled into Qt-Core-only targets.
using SystemDarkProbe = bool();
void setSystemDarkProbe(SystemDarkProbe *probe);
[[nodiscard]] bool currentAppearanceIsDark();

/// The palette for one appearance.
[[nodiscard]] const ThemePalette &themePalette(bool dark);

/// The default folder colour for newly created folders (WebKit: moss).
[[nodiscard]] QString defaultFolderColor();

/// Style sheet for the main window and its sidebar.
[[nodiscard]] QString windowStyleSheet(bool dark);

/// Shared style sheet for dialogs and popups.
[[nodiscard]] QString sheetStyleSheet(bool dark);

} // namespace yobro::spike
