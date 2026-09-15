#include "spike/Theme.hpp"

namespace yobro::spike {
namespace {

const ThemePalette darkPalette{
    .shell = QStringLiteral("#172019"),
    .sidebarTop = QStringLiteral("#1d2821"),
    .sidebarBottom = QStringLiteral("#172019"),
    .surface = QStringLiteral("#303a33"),
    .surfaceRaised = QStringLiteral("#273229"),
    .surfaceHover = QStringLiteral("#354239"),
    .border = QStringLiteral("#303d33"),
    .borderStrong = QStringLiteral("#445146"),
    .text = QStringLiteral("#e3e9e0"),
    .textMuted = QStringLiteral("#a2aaa1"),
    .accent = QStringLiteral("#f46a35"),
    .selection = QStringLiteral("#39433b"),
    .sheetBackground = QStringLiteral("#1b231d"),
    .sheetSurface = QStringLiteral("#232d26"),
    .sheetHover = QStringLiteral("#2d3830"),
    .sheetBorder = QStringLiteral("#354239"),
    .sheetText = QStringLiteral("#e3e9e0"),
    .sheetMuted = QStringLiteral("#a2aaa1"),
    .sheetSelection = QStringLiteral("#33402f"),
};

const ThemePalette lightPalette{
    .shell = QStringLiteral("#eef0ea"),
    .sidebarTop = QStringLiteral("#e6e9e1"),
    .sidebarBottom = QStringLiteral("#eef0ea"),
    .surface = QStringLiteral("#ffffff"),
    .surfaceRaised = QStringLiteral("#f7f8f4"),
    .surfaceHover = QStringLiteral("#e2e6dd"),
    .border = QStringLiteral("#d5dbd2"),
    .borderStrong = QStringLiteral("#bcc5b8"),
    .text = QStringLiteral("#232b25"),
    .textMuted = QStringLiteral("#5d6b5f"),
    .accent = QStringLiteral("#c0521f"),
    .selection = QStringLiteral("#dde5d8"),
    .sheetBackground = QStringLiteral("#f9f8f4"),
    .sheetSurface = QStringLiteral("#ffffff"),
    .sheetHover = QStringLiteral("#eef1ea"),
    .sheetBorder = QStringLiteral("#dce2da"),
    .sheetText = QStringLiteral("#303b3b"),
    .sheetMuted = QStringLiteral("#5d6b5f"),
    .sheetSelection = QStringLiteral("#f0f4ed"),
};

} // namespace

const ThemePalette &themePalette(bool dark) {
    return dark ? darkPalette : lightPalette;
}

QString windowStyleSheet(bool dark) {
    const ThemePalette &colors = themePalette(dark);
    return QStringLiteral(
        "#productShell{background:%1;color:%8;}"
        "#workspaceSidebar{background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %2,stop:1 %3);border:0;border-radius:0;}"
        "#compactSidebar{background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %2,stop:1 %3);border:0;}"
        "#compactSidebar QPushButton{background:transparent;border:0;border-radius:9px;color:%9;font-size:17px;min-width:42px;min-height:40px;}"
        "#compactSidebar QPushButton:hover{background:%6;color:%8;}"
        "#compactTabs{background:transparent;border:0;outline:0;}"
        "#compactTabs::item{border-radius:9px;margin:2px 0;padding:6px;color:%8;}"
        "#compactTabs::item:hover{background:%6;}"
        "#compactTabs::item:selected{background:%12;}"
        "#browserContentHost{background:%1;border:0;}"
        "#topChrome{background:transparent;border:0;min-height:52px;}"
        "#topNavigation{background:transparent;border:0;border-radius:0;padding:0;}"
        "#topNavigation QPushButton{font-size:22px;font-weight:300;min-width:36px;min-height:42px;padding:0;color:%9;}"
        "#topAddress{background:%4;border:1px solid %7;border-radius:14px;padding:13px 16px;color:%8;selection-background-color:%11;font-size:14px;font-weight:600;}"
        "#topAddress::placeholder{color:%9;}"
        "#libraryButton{background:%5;border:1px solid %7;border-radius:13px;min-height:42px;text-align:left;padding:0 14px;color:%8;font-size:15px;font-weight:600;}"
        "#sidebarQuickSearch{background:%5;border:1px solid %7;border-radius:14px;min-height:47px;text-align:left;padding:0 14px;color:%9;font-size:13px;font-weight:600;}"
        "#sidebarQuickSearch:hover{background:%4;border-color:%11;}"
        "#sidebarTabActions QPushButton{background:transparent;border:0;text-align:left;color:%9;font-size:13px;min-height:32px;padding:6px 8px;}"
        "#sidebarSpaceActions,#sidebarFolderActions{background:%5;border:1px solid %7;border-radius:13px;padding:5px;}"
        "#sidebarPageActions{background:%5;border:1px solid %7;border-radius:13px;padding:5px;}"
        "#sidebarLibraryActions{background:transparent;border:0;border-radius:0;padding:0;}"
        "#sidebarLibraryActions QPushButton{color:%9;font-size:12px;font-weight:600;min-height:30px;text-align:left;padding:6px 8px;}"
        "#sidebarAgentCard{background:%5;border:1px solid %7;border-radius:15px;}"
        "#agentPaneToggle{background:transparent;border:0;color:%8;font-size:12px;font-weight:600;text-align:left;padding:0;}"
        "#agentPaneToggle:hover{background:transparent;border:0;color:%8;}"
        "#sidebarProfileFooter{color:%9;font-size:12px;font-weight:600;padding:7px 8px;background:transparent;border:0;text-align:left;}"
        "#sidebarProfileFooter:hover{color:%8;background:%6;}"
        "#workspaceTree{background:transparent;border:0;padding:0;color:%8;outline:0;}"
        "#workspaceTree::item{padding:10px 10px;border-radius:12px;margin:3px 0;min-height:25px;}"
        "#workspaceTree::item:hover{background:%6;}"
        "#workspaceTree::item:selected{background:%12;color:%8;}"
        "#workspaceTree::branch{background:transparent;}"
        "QTabWidget::pane{background:%1;border:1px solid %7;border-radius:20px;margin-top:0;}"
        "QLineEdit,QPlainTextEdit{background:%4;border:1px solid %7;border-radius:10px;color:%8;}"
        "QPushButton{padding:7px 9px;border:1px solid transparent;border-radius:9px;background:transparent;color:%8;}"
        "QPushButton:hover{background:%6;border-color:%11;}"
        "QPushButton:pressed{background:%6;border-color:%10;}"
        "QPushButton:disabled{color:%9;background:transparent;}"
        "QComboBox{background:%4;border:0;border-radius:9px;padding:7px 8px;color:%8;font-weight:600;}"
        "QComboBox::drop-down{border:0;width:20px;}"
        "#browserStatus{color:%9;padding:3px 9px;font-size:11px;}"
        "#findBar{background:%5;border:1px solid %7;border-radius:12px;}"
        "#findStatus{color:%9;font-size:11px;}"
        "#agentPane{background:%5;color:%8;}"
    ).arg(
        colors.shell,          // %1
        colors.sidebarTop,     // %2
        colors.sidebarBottom,  // %3
        colors.surface,        // %4
        colors.surfaceRaised,  // %5
        colors.surfaceHover,   // %6
        colors.border,         // %7
        colors.text,           // %8
        colors.textMuted       // %9
    ).arg(
        colors.accent,         // %10
        colors.borderStrong,   // %11
        colors.selection       // %12
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
        "QLineEdit{background:%2;border:1px solid %4;color:%5;border-radius:10px;padding:11px 12px;}"
        "QListWidget{background:transparent;border:0;outline:0;}"
        "QListWidget::item{background:%2;border:1px solid %4;border-radius:12px;margin:3px 0;padding:11px;color:%5;}"
        "QListWidget::item:selected{background:%7;border-color:%4;color:%5;}"
        "QPlainTextEdit,QTextEdit{background:%2;border:1px solid %4;border-radius:10px;color:%5;}"
        "#settingsSectionTitle{font:600 13px -apple-system;color:%6;}"
        "#settingsPath{font:10px ui-monospace,monospace;color:%6;}"
        "#librarySegment{background:%3;border-radius:12px;padding:4px;}"
        "#librarySegment QPushButton{font-weight:600;min-height:34px;}"
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
