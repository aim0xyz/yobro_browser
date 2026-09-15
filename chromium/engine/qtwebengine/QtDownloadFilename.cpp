#include "engine/qtwebengine/QtDownloadFilename.hpp"

#include <QFileInfo>
#include <QTextBoundaryFinder>

namespace yobro::qtwebengine {
namespace {

QString firstGraphemes(QString value, int limit) {
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, value);
    boundaries.toStart();
    qsizetype boundary = 0;
    int count = 0;
    while (count < limit) {
        const qsizetype next = boundaries.toNextBoundary();
        if (next < 0) return value;
        boundary = next;
        ++count;
    }
    return boundary < value.size() ? value.left(boundary) : value;
}

} // namespace

QString sanitizedDownloadFilename(QString suggestedName) {
    const QString name = QFileInfo(suggestedName).fileName();
    QString cleaned;
    cleaned.reserve(name.size());
    const QList<uint> scalars = name.toUcs4();
    for (const uint encoded : scalars) {
        const char32_t scalar = static_cast<char32_t>(encoded);
        const QChar::Category category = QChar::category(scalar);
        if (category == QChar::Other_Control || category == QChar::Other_Format)
            continue;
        cleaned.append(QString::fromUcs4(&scalar, 1));
    }
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty() || cleaned == QStringLiteral(".") || cleaned == QStringLiteral(".."))
        cleaned = QStringLiteral("Download");
    if (cleaned.startsWith('.'))
        cleaned.prepend(QStringLiteral("Download"));
    return firstGraphemes(std::move(cleaned), 180);
}

} // namespace yobro::qtwebengine
