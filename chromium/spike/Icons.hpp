#pragma once

#include <QIcon>
#include <QString>

class QAbstractButton;

namespace yobro::spike {

/// Tintable chrome icons. The SVG set is Lucide (ISC license, see
/// icons/LICENSE-LUCIDE.txt), mapped onto the SF Symbols the WebKit build
/// shows. Buttons keep their icons across appearance changes: `retintIcons`
/// re-renders every installed icon when the palette flips.
enum class IconRole {
    normal, // ink — primary chrome
    muted,  // textMuted — quiet affordances
    accent, // moss — accent moments
    brand,  // brandOrange — agent and brand moments
};

/// Paints `name` (icon file name without extension) onto `button` in the role
/// colour. The button keeps its text; icon and text render side by side.
void installIcon(QAbstractButton *button, const QString &name, IconRole role);

/// Re-renders every installed button icon against the current palette.
void retintIcons(bool dark);

} // namespace yobro::spike
