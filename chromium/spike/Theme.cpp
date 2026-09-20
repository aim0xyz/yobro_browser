#include "spike/Theme.hpp"

#include <QtGlobal>

namespace yobro::spike {
namespace {

/// Pure Qt-Core colour helpers: this file is also compiled into the
/// Qt-Core-only bridge-policy library, so QColor/QGuiApplication are off-limits.
bool parseHex(const QString &hex, int &red, int &green, int &blue) {
    const QString trimmed = hex.trimmed();
    if (trimmed.length() != 7 || trimmed.at(0) != QLatin1Char('#')) return false;
    bool okRed = false;
    bool okGreen = false;
    bool okBlue = false;
    red = trimmed.mid(1, 2).toInt(&okRed, 16);
    green = trimmed.mid(3, 2).toInt(&okGreen, 16);
    blue = trimmed.mid(5, 2).toInt(&okBlue, 16);
    return okRed && okGreen && okBlue;
}

QString toHexString(int red, int green, int blue) {
    return QStringLiteral("#%1%2%3")
        .arg(red, 2, 16, QLatin1Char('0'))
        .arg(green, 2, 16, QLatin1Char('0'))
        .arg(blue, 2, 16, QLatin1Char('0'));
}

/// The WebKit views build hover, selection and secondary colours with
/// `.opacity()` on top of the surface beneath. These helpers do the same
/// blending here so the style sheets receive one concrete colour per token.
QString blend(const QString &foreground, double alpha, const QString &background) {
    int foreRed = 0;
    int foreGreen = 0;
    int foreBlue = 0;
    int backRed = 0;
    int backGreen = 0;
    int backBlue = 0;
    if (!parseHex(foreground, foreRed, foreGreen, foreBlue)) return background;
    if (!parseHex(background, backRed, backGreen, backBlue)) return background;
    const auto mix = [alpha](int fore, int back) {
        return qRound(static_cast<double>(back) * (1.0 - alpha) + static_cast<double>(fore) * alpha);
    };
    return toHexString(mix(foreRed, backRed), mix(foreGreen, backGreen), mix(foreBlue, backBlue));
}

QString rgba(const QString &hex, double alpha) {
    int red = 0;
    int green = 0;
    int blue = 0;
    if (!parseHex(hex, red, green, blue)) return hex;
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(red)
        .arg(green)
        .arg(blue)
        .arg(alpha, 0, 'f', 3);
}

/// The WebKit build's source tokens (Sources/YOBRO/Theme.swift), one row per
/// appearance. The `chromium-theme-tokens` test reads the `/*name*/` comments
/// and fails when these values and Theme.swift drift apart.
struct WebKitTokens {
    const char *text;
    const char *accent;
    const char *page;
    const char *surface;
    const char *chromeTop;
    const char *chromeBottom;
    const char *sage;
    const char *lilac;
    const char *peach;
    const char *folderSage;
    const char *folderLilac;
    const char *folderPeach;
    const char *folderSky;
    const char *folderSand;
    const char *folderRose;
    const char *field;
    const char *border;
    const char *brandOrange;
};

constexpr WebKitTokens lightTokens{
    /*text*/        "#303B3B",
    /*accent*/      "#4F6654",
    /*page*/        "#F9F8F4",
    /*surface*/     "#FFFFFF",
    /*chromeTop*/   "#E3E6D6",
    /*chromeBottom*/ "#D6DED4",
    /*sage*/        "#DBE6D4",
    /*lilac*/       "#E0DBEB",
    /*peach*/       "#F2DECC",
    /*folderSage*/  "#71896F",
    /*folderLilac*/ "#887CA0",
    /*folderPeach*/ "#B47E5D",
    /*folderSky*/   "#66888E",
    /*folderSand*/  "#9D8756",
    /*folderRose*/  "#997078",
    /*field*/       "#F4F5EF",
    /*border*/      "#CAD3C8",
    /*brandOrange*/ "#FF541C",
};

constexpr WebKitTokens darkTokens{
    /*text*/        "#E3E9E0",
    /*accent*/      "#ADC6A8",
    /*page*/        "#191E1B",
    /*surface*/     "#39433B",
    /*chromeTop*/   "#252E28",
    /*chromeBottom*/ "#1B241F",
    /*sage*/        "#344735",
    /*lilac*/       "#40384E",
    /*peach*/       "#513E30",
    /*folderSage*/  "#A7C1A3",
    /*folderLilac*/ "#BAACCD",
    /*folderPeach*/ "#D6A07D",
    /*folderSky*/   "#92B9BF",
    /*folderSand*/  "#CDB77D",
    /*folderRose*/  "#C79AA3",
    /*field*/       "#303A33",
    /*border*/      "#536157",
    /*brandOrange*/ "#FF6A38",
};

ThemePalette buildPalette(const WebKitTokens &tokens) {
    ThemePalette palette;
    palette.ink = QString::fromLatin1(tokens.text);
    palette.moss = QString::fromLatin1(tokens.accent);
    palette.paper = QString::fromLatin1(tokens.page);
    palette.surface = QString::fromLatin1(tokens.surface);
    palette.chromeTop = QString::fromLatin1(tokens.chromeTop);
    palette.chromeBottom = QString::fromLatin1(tokens.chromeBottom);
    palette.sage = QString::fromLatin1(tokens.sage);
    palette.lilac = QString::fromLatin1(tokens.lilac);
    palette.peach = QString::fromLatin1(tokens.peach);
    palette.folderSage = QString::fromLatin1(tokens.folderSage);
    palette.folderLilac = QString::fromLatin1(tokens.folderLilac);
    palette.folderPeach = QString::fromLatin1(tokens.folderPeach);
    palette.folderSky = QString::fromLatin1(tokens.folderSky);
    palette.folderSand = QString::fromLatin1(tokens.folderSand);
    palette.folderRose = QString::fromLatin1(tokens.folderRose);
    palette.field = QString::fromLatin1(tokens.field);
    palette.border = QString::fromLatin1(tokens.border);
    palette.brandOrange = QString::fromLatin1(tokens.brandOrange);

    // Sidebar washes sit on the chrome gradient, popover and sheet washes on
    // paper — the same surfaces the WebKit views composite over.
    palette.textMuted = blend(palette.ink, 0.55, palette.paper);
    palette.hoverWash = blend(palette.ink, 0.065, palette.chromeBottom);
    palette.pressedWash = blend(palette.ink, 0.12, palette.chromeBottom);
    palette.selectionWash = blend(palette.moss, 0.14, palette.chromeBottom);
    palette.mossTintWash = blend(palette.moss, 0.10, palette.paper);
    palette.borderHairline = rgba(palette.ink, 0.08);
    palette.borderSoft = rgba(palette.border, 0.48);
    palette.borderField = rgba(palette.border, 0.65);

    palette.shell = palette.paper;
    palette.sidebarTop = palette.chromeTop;
    palette.sidebarBottom = palette.chromeBottom;
    palette.text = palette.ink;
    palette.surfaceRaised = palette.field;
    palette.surfaceHover = palette.hoverWash;
    palette.surfacePressed = palette.pressedWash;
    palette.surfaceAgent = rgba(palette.surface, 0.32);
    palette.surfaceLibrary = rgba(palette.surface, 0.20);
    palette.surfaceSearchRow = rgba(palette.surface, 0.50);
    palette.spaceChatPanel = rgba(palette.chromeBottom, 0.90);
    palette.surfaceTreeSelected = rgba(palette.surface, 0.75);
    palette.surfaceTreeHover = rgba(palette.surface, 0.24);
    palette.surfacePopupButton = rgba(palette.surface, 0.72);
    palette.borderStrong = palette.border;
    palette.accent = palette.moss;
    palette.selection = palette.selectionWash;
    palette.sheetBackground = palette.paper;
    palette.sheetSurface = palette.surface;
    palette.sheetHover = palette.mossTintWash;
    palette.sheetBorder = palette.borderField;
    palette.sheetText = palette.ink;
    palette.sheetMuted = palette.textMuted;
    palette.sheetSelection = palette.mossTintWash;
    return palette;
}

QString gradient() {
    return QStringLiteral(
        "qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %1,stop:1 %2)");
}

} // namespace

namespace {

SystemDarkProbe *systemDarkProbe = nullptr;

} // namespace

void setSystemDarkProbe(SystemDarkProbe *probe) {
    systemDarkProbe = probe;
}

bool currentAppearanceIsDark() {
    // Golden-reference captures pin the appearance so a capture run never
    // depends on, or changes, the user's global macOS setting.
    static const int pinned = [] {
        const QString override = qEnvironmentVariable("YOBRO_TEST_APPEARANCE");
        if (override.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0) return 1;
        if (override.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) return -1;
        return 0;
    }();
    if (pinned == 1) return true;
    if (pinned == -1) return false;
    // Without a probe (some test harnesses) the palette falls back to light.
    return systemDarkProbe ? systemDarkProbe() : false;
}

const ThemePalette &themePalette(bool dark) {
    static const ThemePalette light = buildPalette(lightTokens);
    static const ThemePalette darkPalette = buildPalette(darkTokens);
    return dark ? darkPalette : light;
}

QString defaultFolderColor() {
    // WebKit's first folder colour is moss (FolderColor.moss).
    return QStringLiteral("#4F6654");
}

QString windowStyleSheet(bool dark) {
    const ThemePalette &colors = themePalette(dark);
    // Place markers are contiguous and all used: Qt's multi-arg .arg() fills
    // the lowest markers present in order, so gaps would shift every colour.
    return QStringLiteral(
        // One chrome gradient behind everything, exactly like the WebKit shell.
        "#productShell{background:%1;color:%6;}"
        "#workspaceSidebar{background:transparent;border:0;}"
        "#compactSidebar{background:transparent;border:0;}"
        "#compactSidebar QPushButton{background:transparent;border:0;border-radius:9px;color:%7;font-size:17px;min-width:42px;min-height:40px;}"
        "#compactSidebar QPushButton:hover{background:%4;color:%6;}"
        "#compactTabs{background:transparent;border:0;outline:0;}"
        "#compactTabs::item{border-radius:9px;margin:2px 0;padding:6px;color:%6;}"
        "#compactTabs::item:hover{background:%4;}"
        "#compactTabs::item:selected{background:%9;}"
        "#browserContentHost{background:transparent;border:0;}"
        // Sidebar, top down: navigation row, brand, search field.
        "#topNavigation{background:transparent;border:0;border-radius:0;padding:0;}"
        "#topNavigation QPushButton{font-size:15px;min-width:30px;min-height:30px;padding:0;border-radius:9px;color:%6;}"
        "#productBrand{font-family:'SF Pro Rounded','SF Pro Text',-apple-system;font-size:29px;font-weight:600;letter-spacing:-1.4px;color:%6;}"
        "#productPreview{color:%17;font-family:'SF Mono',Menlo,monospace;font-weight:700;font-size:8px;letter-spacing:1px;}"
        "#productBrandMark{font-family:'SF Pro Rounded','SF Pro Text',-apple-system;font-size:30px;color:%8;font-weight:800;}"
        "#sidebarSearchRow{background:%15;border:1px solid %10;border-radius:12px;}"
        "#topAddress{background:transparent;border:0;border-radius:10px;padding:9px 2px;color:%6;selection-background-color:%9;font-size:13px;font-weight:600;}"
        "#loginFillButton,#appearanceButton{background:transparent;border:0;border-radius:7px;min-width:26px;min-height:26px;padding:0;color:%7;font-size:13px;}"
        "#loginFillButton:hover,#appearanceButton:hover{background:%4;color:%6;}"
        "#sidebarMailButton{background:transparent;border:0;border-radius:9px;min-height:34px;padding:6px 9px;color:%6;text-align:left;font-size:12px;font-weight:600;}"
        "#sidebarMailButton:hover{background:%4;}"
        "#spaceStrip{background:%14;border:1px solid %10;border-radius:10px;min-height:32px;max-height:32px;padding:0 6px;}"
        "#spaceStripButton{background:transparent;border:0;color:%6;font-size:11px;font-weight:600;text-align:left;padding:0 4px;}"
        "#spaceStripButton:hover{background:transparent;}"
        "#spaceStripAddButton{background:transparent;border:0;border-radius:6px;min-width:22px;max-width:22px;min-height:22px;max-height:22px;padding:0;color:%7;font-size:14px;}"
        "#spaceStripAddButton:hover{background:%4;color:%6;}"
        "#sidebarSectionLabel{color:%7;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;font-weight:600;font-size:9px;letter-spacing:1.4px;padding:23px 0px 9px 10px;}"
        "#workspaceTree{background:transparent;border:0;padding:0;color:%6;outline:0;}"
        "#workspaceTree::item{padding:5px 9px;border-radius:9px;margin:2px 0;min-height:38px;}"
        "#workspaceTree::item:hover{background:%18;}"
        "#workspaceTree::item:selected{background:%19;color:%6;}"
        "#workspaceTree::branch{background:transparent;}"
        // Sidebar action rows and library icons.
        "#sidebarTabActions QPushButton,#sidebarNewTabButton,#sidebarNewNoteButton,#sidebarPrivateTabButton{background:transparent;border:0;text-align:left;color:%7;font-size:12px;min-height:28px;padding:4px 8px;border-radius:8px;font-weight:500;}"
        "#sidebarTabActions QPushButton:hover,#sidebarNewTabButton:hover,#sidebarNewNoteButton:hover,#sidebarPrivateTabButton:hover{background:%4;color:%6;}"
        "#sidebarSpaceActions,#sidebarFolderActions{background:transparent;border:0;border-radius:0;padding:0;}"
        "#sidebarPageActions{background:transparent;border:0;border-radius:0;padding:0;}"
        "#sidebarPageActions QPushButton{background:transparent;border:0;border-radius:8px;color:%7;min-height:28px;}"
        "#sidebarPageActions QPushButton:hover{background:%4;color:%6;}"
        "#sidebarLibraryActions,#sidebarLibraryBar{background:%13;border:0;border-radius:10px;padding:3px;}"
        "#sidebarLibraryActions QPushButton,#sidebarLibraryBar QPushButton{color:%6;font-size:12px;font-weight:600;min-height:28px;text-align:center;border-radius:8px;background:transparent;border:0;}"
        "#sidebarLibraryActions QPushButton:hover,#sidebarLibraryBar QPushButton:hover{background:%4;}"
        "#libraryButton{background:transparent;border:0;min-height:30px;text-align:center;padding:0 8px;color:%6;font-size:12px;font-weight:600;border-radius:8px;}"
        "#sidebarAgentCard{background:%14;border:1px solid %10;border-radius:12px;padding:8px 10px;}"
        "#agentPaneToggle{background:transparent;border:0;color:%6;font-size:12px;font-weight:600;text-align:left;padding:0;}"
        "#agentPaneToggle:hover{background:transparent;border:0;color:%6;}"
        "#sidebarProfileFooter{color:%7;font-size:12px;font-weight:600;padding:7px 8px;background:transparent;border:0;text-align:left;border-radius:9px;}"
        "#sidebarProfileFooter:hover{color:%6;background:%4;}"
        // The workspace sits on the gradient as a paper card with a hairline,
        // the way the WebKit shell clips its content views.
        "QTabWidget::pane{background:%3;border:1px solid %10;border-radius:14px;margin-top:0;}"
        "QLineEdit,QPlainTextEdit{background:%2;border:1px solid %11;border-radius:10px;color:%6;}"
        "QPushButton{padding:7px 9px;border:1px solid transparent;border-radius:9px;background:transparent;color:%6;}"
        "QPushButton:hover{background:%4;border-color:%12;}"
        "QPushButton:pressed{background:%5;border-color:%12;}"
        "QPushButton:disabled{color:%7;background:transparent;}"
        "QComboBox{background:%20;border:1px solid %12;border-radius:9px;padding:7px 8px;color:%6;font-weight:600;}"
        "QComboBox::drop-down{border:0;width:20px;}"
        "#browserStatus{color:%7;padding:2px 9px;font-size:11px;}"
        "#findBar{background:%3;border:1px solid %11;border-radius:12px;}"
        "#findStatus{color:%7;font-size:11px;}"
        "#agentPane{background:transparent;color:%6;}"
        "#agentPaneLabel{color:%8;font-family:'SF Mono',Menlo,monospace;font-weight:700;font-size:10px;letter-spacing:0.8px;}"
        "#agentWebHost{background:%3;border:1px solid %10;border-radius:14px;}"
        "#agentQuickAccess{background:%2;border:1px solid %17;border-radius:6px;color:%17;font-family:'SF Pro Rounded','SF Pro Text',-apple-system;font-size:7px;font-weight:700;letter-spacing:0.4px;padding:8px 0px;margin-right:2px;}"
        "#agentQuickAccess:checked{background:%17;color:%3;border:0;}"
        "#agentQuickAccess:hover{background:%4;}"

        "#spaceChatPanel{background:%16;border-radius:0;}"
        "#spaceChatHeader{background:transparent;border-bottom:1px solid %10;padding:22px 10px 10px 10px;}"
        "#spaceChatAgentLabel{color:%17;font-family:'SF Pro Rounded','SF Pro Text',-apple-system;font-size:9px;font-weight:700;letter-spacing:1.3px;}"
        "#spaceChatHeading{font-family:'SF Pro Rounded','SF Pro Text',-apple-system;font-size:16px;font-weight:600;color:%6;}"
        "#spaceChatStatusPill{background:%14;border:1px solid %10;border-radius:12px;padding:2px 8px;font-size:9px;font-weight:600;color:%6;}"
        "#spaceChatCircleButton{background:%14;border:0;border-radius:15px;min-width:30px;max-width:30px;min-height:30px;max-height:30px;padding:0;color:%6;}"
        "#spaceChatCircleButton:hover{background:%4;}"
        "#spaceChatTimeline{background:transparent;border:0;outline:0;color:%6;}"
        "#spaceChatTimeline::item{border-radius:13px;margin:3px 0;padding:8px 10px;background:transparent;}"
        "#spaceChatTimeline::item:hover{background:transparent;}"
        "#spaceChatTimeline::item:selected{background:transparent;}"
        "#spaceChatComposerCard{background:%2;border:1px solid %10;border-radius:14px;padding:8px 10px;}"
        "#spaceChatInput{background:transparent;border:0;color:%6;font-size:12px;}"
        "#spaceChatSendButton{background:%17;border:0;border-radius:15px;min-width:30px;max-width:30px;min-height:30px;max-height:30px;padding:0;color:%3;font-weight:700;}"
        "#spaceChatSendButton:disabled{background:%14;color:%7;}"
        "#spaceChatNotice{background:%14;border:1px solid %10;border-radius:9px;padding:8px 10px;color:%8;font-size:9px;font-weight:500;}"
        "#spaceChatNoticeLocal{background:%14;border:1px solid %10;border-radius:9px;padding:8px 10px;color:%17;font-size:9px;font-weight:500;}"
        "#spaceChatDisclosureHeader{background:transparent;border:0;text-align:left;color:%6;font-size:10px;font-weight:500;padding:4px 0;}"
        "#spaceChatDisclosureContent{color:%7;font-size:9px;padding:2px 0 6px 16px;}"
    ).arg(
        gradient().arg(colors.chromeTop, colors.chromeBottom),  // %1 window gradient
        colors.surface,        // %2
        colors.paper,          // %3
        colors.hoverWash,      // %4
        colors.pressedWash,    // %5
        colors.ink,            // %6
        colors.textMuted,      // %7
        colors.brandOrange,    // %8
        colors.selectionWash,  // %9
        colors.borderHairline, // %10
        colors.borderField,    // %11
        colors.borderSoft,     // %12
        colors.surfaceLibrary, // %13
        colors.surfaceAgent,   // %14
        colors.surfaceSearchRow, // %15
        colors.spaceChatPanel, // %16
        colors.moss,           // %17
        colors.surfaceTreeHover, // %18
        colors.surfaceTreeSelected, // %19
        colors.surfacePopupButton  // %20
    );
}

QString sheetStyleSheet(bool dark) {
    const ThemePalette &colors = themePalette(dark);
    return QStringLiteral(
        "QDialog{background:%1;color:%5;}"
        "QLabel{color:%5;}"
        "QCheckBox{color:%5;}"
        "QPushButton{background:%2;border:1px solid %4;border-radius:9px;padding:8px 12px;color:%5;}"
        "QPushButton:hover{background:%3;}"
        "QPushButton:disabled{color:%6;}"
        "#sheetTitle{font-family:'New York',Georgia,serif;font-size:22px;color:%5;}"
        "QLineEdit{background:%2;border:1px solid %4;color:%5;border-radius:10px;padding:11px 12px;}"
        "QListWidget{background:transparent;border:0;outline:0;}"
        "QListWidget::item{background:%2;border:1px solid %4;border-radius:12px;margin:3px 0;padding:11px;color:%5;}"
        "QListWidget::item:selected{background:%7;border-color:%4;color:%5;}"
        "QPlainTextEdit,QTextEdit{background:%2;border:1px solid %4;border-radius:10px;color:%5;}"
        "#settingsSectionTitle{font:600 13px -apple-system;color:%6;}"
        "#settingsPath{font:10px Menlo;color:%6;}"
        "#librarySegment{background:%3;border-radius:12px;padding:4px;}"
        "#librarySegment QPushButton{font-weight:600;min-height:34px;}"
        "#segmentActive{background:%2;border-radius:9px;color:%5;}"
    ).arg(
        colors.sheetBackground,  // %1
        colors.sheetSurface,     // %2
        colors.sheetHover,       // %3
        colors.sheetBorder,      // %4
        colors.sheetText,        // %5
        colors.sheetMuted,       // %6
        colors.sheetSelection    // %7
    );
}

} // namespace yobro::spike
