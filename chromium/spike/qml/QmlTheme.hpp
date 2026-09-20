#pragma once

#include <QColor>
#include <QObject>
#include <QUrl>

namespace yobro::spike {

/// The design tokens for the QML shell, read live from the shared Theme layer.
///
/// The QML files carry no colours of their own: every token comes from
/// `themePalette()`, which `chromium-theme-tokens` already pins to
/// `Sources/YOBRO/Theme.swift`. A system appearance flip re-emits every
/// property, so bindings retint without a restart.
class QmlTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dark READ dark NOTIFY paletteChanged)
    Q_PROPERTY(QColor ink READ ink NOTIFY paletteChanged)
    Q_PROPERTY(QColor moss READ moss NOTIFY paletteChanged)
    Q_PROPERTY(QColor paper READ paper NOTIFY paletteChanged)
    Q_PROPERTY(QColor surface READ surface NOTIFY paletteChanged)
    Q_PROPERTY(QColor chromeTop READ chromeTop NOTIFY paletteChanged)
    Q_PROPERTY(QColor chromeBottom READ chromeBottom NOTIFY paletteChanged)
    Q_PROPERTY(QColor sage READ sage NOTIFY paletteChanged)
    Q_PROPERTY(QColor lilac READ lilac NOTIFY paletteChanged)
    Q_PROPERTY(QColor peach READ peach NOTIFY paletteChanged)
    Q_PROPERTY(QColor field READ field NOTIFY paletteChanged)
    Q_PROPERTY(QColor border READ border NOTIFY paletteChanged)
    Q_PROPERTY(QColor brandOrange READ brandOrange NOTIFY paletteChanged)
    Q_PROPERTY(QColor textMuted READ textMuted NOTIFY paletteChanged)
    Q_PROPERTY(QColor hoverWash READ hoverWash NOTIFY paletteChanged)
    Q_PROPERTY(QColor pressedWash READ pressedWash NOTIFY paletteChanged)
    Q_PROPERTY(QColor selectionWash READ selectionWash NOTIFY paletteChanged)
    Q_PROPERTY(QColor mossTintWash READ mossTintWash NOTIFY paletteChanged)
    Q_PROPERTY(QColor borderHairline READ borderHairline NOTIFY paletteChanged)
    Q_PROPERTY(QColor borderSoft READ borderSoft NOTIFY paletteChanged)
    Q_PROPERTY(QColor borderField READ borderField NOTIFY paletteChanged)
    /// The brand wordmark, body and micro-caps families. macOS hides the SF
    /// families from non-Apple processes, so these resolve to the system font
    /// or Menlo unless the user installed the retail SF fonts.
    Q_PROPERTY(QString fontRounded READ fontRounded CONSTANT)
    Q_PROPERTY(QString fontText READ fontText CONSTANT)
    Q_PROPERTY(QString fontMono READ fontMono CONSTANT)

public:
    explicit QmlTheme(QObject *parent = nullptr);

    [[nodiscard]] bool dark() const;
    [[nodiscard]] QColor ink() const;
    [[nodiscard]] QColor moss() const;
    [[nodiscard]] QColor paper() const;
    [[nodiscard]] QColor surface() const;
    [[nodiscard]] QColor chromeTop() const;
    [[nodiscard]] QColor chromeBottom() const;
    [[nodiscard]] QColor sage() const;
    [[nodiscard]] QColor lilac() const;
    [[nodiscard]] QColor peach() const;
    [[nodiscard]] QColor field() const;
    [[nodiscard]] QColor border() const;
    [[nodiscard]] QColor brandOrange() const;
    [[nodiscard]] QColor textMuted() const;
    [[nodiscard]] QColor hoverWash() const;
    [[nodiscard]] QColor pressedWash() const;
    [[nodiscard]] QColor selectionWash() const;
    [[nodiscard]] QColor mossTintWash() const;
    [[nodiscard]] QColor borderHairline() const;
    [[nodiscard]] QColor borderSoft() const;
    [[nodiscard]] QColor borderField() const;
    [[nodiscard]] static QString fontRounded();
    [[nodiscard]] static QString fontText();
    [[nodiscard]] static QString fontMono();

    /// Tintable Lucide icon as a provider URL; the dark flag is part of the
    /// URL so an appearance flip re-renders instead of serving a stale cache.
    Q_INVOKABLE [[nodiscard]] QUrl iconUrl(const QString &name, const QString &role = QStringLiteral("ink")) const;

    /// The shared localization: German text, or its English translation when
    /// the system (or `YOBRO_LANGUAGE`) says so — same table as the widget shell.
    Q_INVOKABLE [[nodiscard]] QString loc(const QString &german, const QString &english) const;

signals:
    void paletteChanged();

private:
    void watchAppearance();
};

} // namespace yobro::spike
