#pragma once

#include <QString>

namespace yobro::spike {

/// True when the primary system language is German, matching the WebKit build.
///
/// A secondary preferred translation is deliberately ignored: only the first
/// system language decides, and German covers Austria and Switzerland too.
[[nodiscard]] bool isGerman();

/// Returns the German text, or its English translation for other languages.
///
/// Translations come from the same `English.json` the WebKit build uses, plus a
/// Chromium-specific file for strings that only exist in this shell. A missing
/// translation falls back to the German text instead of showing an empty label.
[[nodiscard]] QString L(const QString &german);

/// For interpolated messages, where no lookup table can help.
[[nodiscard]] QString L(const QString &german, const QString &english);

} // namespace yobro::spike
