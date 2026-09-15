#pragma once

#include <QString>

namespace yobro::qtwebengine {

// Mirrors WebKit DownloadStore filename normalization: last path component,
// Unicode control/format removal, whitespace trim, hidden-name prefixing, and
// a 180-grapheme limit.
[[nodiscard]] QString sanitizedDownloadFilename(QString suggestedName);

} // namespace yobro::qtwebengine
