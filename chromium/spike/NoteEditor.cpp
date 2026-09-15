#include "spike/NoteEditor.hpp"

#include "spike/Localization.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextList>
#include <QTextListFormat>
#include <QVBoxLayout>

namespace yobro::spike {
namespace {

QPushButton *toolbarButton(
    QWidget *parent,
    const QString &label,
    const QString &objectName,
    const QString &tooltip
) {
    auto *button = new QPushButton(label, parent);
    button->setObjectName(objectName);
    button->setToolTip(tooltip);
    button->setFixedHeight(26);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

NoteEditor::NoteEditor(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("noteEditor"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(24, 12, 24, 12);
    titleField_ = new QLineEdit(header);
    titleField_->setObjectName(QStringLiteral("noteTitleField"));
    titleField_->setPlaceholderText(L(QStringLiteral("Titel der Notiz"), QStringLiteral("Note title")));
    titleField_->setFrame(false);
    titleField_->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;background:transparent"));
    headerLayout->addWidget(titleField_, 1);
    auto *savedNote = new QLabel(
        L(QStringLiteral("Automatisch gespeichert"), QStringLiteral("Saved automatically")), header);
    savedNote->setObjectName(QStringLiteral("noteSavedNote"));
    savedNote->setStyleSheet(QStringLiteral("font-size:10px"));
    headerLayout->addWidget(savedNote);
    layout->addWidget(header);

    auto *headerLine = new QFrame(this);
    headerLine->setFrameShape(QFrame::HLine);
    layout->addWidget(headerLine);

    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(20, 6, 20, 6);
    toolbarLayout->setSpacing(6);
    auto *bold = toolbarButton(toolbar, QStringLiteral("B"), QStringLiteral("noteBoldButton"),
                               L(QStringLiteral("Fett"), QStringLiteral("Bold")));
    bold->setStyleSheet(QStringLiteral("font-weight:800"));
    auto *italic = toolbarButton(toolbar, QStringLiteral("I"), QStringLiteral("noteItalicButton"),
                                 L(QStringLiteral("Kursiv"), QStringLiteral("Italic")));
    italic->setStyleSheet(QStringLiteral("font-style:italic"));
    auto *underline = toolbarButton(toolbar, QStringLiteral("U"), QStringLiteral("noteUnderlineButton"),
                                    L(QStringLiteral("Unterstrichen"), QStringLiteral("Underline")));
    underline->setStyleSheet(QStringLiteral("text-decoration:underline"));
    auto *heading = toolbarButton(toolbar, QStringLiteral("H"), QStringLiteral("noteHeadingButton"),
                                  L(QStringLiteral("Überschrift"), QStringLiteral("Heading")));
    auto *bullets = toolbarButton(toolbar, QStringLiteral("•"), QStringLiteral("noteBulletButton"),
                                  L(QStringLiteral("Liste"), QStringLiteral("List")));
    auto *clear = toolbarButton(toolbar, QStringLiteral("⌫"), QStringLiteral("noteClearFormatButton"),
                                L(QStringLiteral("Formatierung entfernen"), QStringLiteral("Remove formatting")));
    for (QPushButton *button : {bold, italic, underline, heading, bullets, clear})
        toolbarLayout->addWidget(button);
    toolbarLayout->addStretch();
    layout->addWidget(toolbar);

    auto *toolbarLine = new QFrame(this);
    toolbarLine->setFrameShape(QFrame::HLine);
    layout->addWidget(toolbarLine);

    contentField_ = new QTextEdit(this);
    contentField_->setObjectName(QStringLiteral("noteContentField"));
    contentField_->setAcceptRichText(true);
    // Pasted markup from a website must not bring styling or links into the note.
    contentField_->setTabChangesFocus(false);
    contentField_->setPlaceholderText(
        L(QStringLiteral("Schreib los. Formatierung über die Leiste oben."),
          QStringLiteral("Start writing. Use the bar above for formatting.")));
    layout->addWidget(contentField_, 1);

    QObject::connect(titleField_, &QLineEdit::textChanged, this, [this] { Q_EMIT changed(); });
    QObject::connect(contentField_, &QTextEdit::textChanged, this, [this] { Q_EMIT changed(); });

    QObject::connect(bold, &QPushButton::clicked, this, [this] {
        contentField_->setFontWeight(contentField_->fontWeight() > QFont::Normal ? QFont::Normal : QFont::Bold);
        contentField_->setFocus();
    });
    QObject::connect(italic, &QPushButton::clicked, this, [this] {
        contentField_->setFontItalic(!contentField_->fontItalic());
        contentField_->setFocus();
    });
    QObject::connect(underline, &QPushButton::clicked, this, [this] {
        contentField_->setFontUnderline(!contentField_->fontUnderline());
        contentField_->setFocus();
    });
    QObject::connect(heading, &QPushButton::clicked, this, [this] {
        // A heading is a larger, bold paragraph; that survives the HTML round trip.
        const bool isHeading = contentField_->fontPointSize() > 15;
        QTextCharFormat format;
        format.setFontPointSize(isHeading ? 13 : 19);
        format.setFontWeight(isHeading ? QFont::Normal : QFont::Bold);
        QTextCursor cursor = contentField_->textCursor();
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.mergeCharFormat(format);
        contentField_->setFocus();
    });
    QObject::connect(bullets, &QPushButton::clicked, this, [this] {
        QTextCursor cursor = contentField_->textCursor();
        if (cursor.currentList()) {
            // Leaving a list turns the item back into a normal paragraph.
            QTextBlockFormat block = cursor.blockFormat();
            block.setObjectIndex(-1);
            block.setIndent(0);
            cursor.setBlockFormat(block);
        } else {
            cursor.createList(QTextListFormat::ListDisc);
        }
        contentField_->setFocus();
    });
    QObject::connect(clear, &QPushButton::clicked, this, [this] {
        QTextCursor cursor = contentField_->textCursor();
        if (!cursor.hasSelection()) cursor.select(QTextCursor::Document);
        cursor.setCharFormat({});
        contentField_->setFocus();
    });
}

QString NoteEditor::title() const {
    return titleField_ ? titleField_->text() : QString();
}

void NoteEditor::setTitle(const QString &value) {
    if (!titleField_) return;
    const QSignalBlocker blocker(titleField_);
    titleField_->setText(value);
}

QString NoteEditor::html() const {
    return contentField_ ? contentField_->toHtml() : QString();
}

void NoteEditor::setHtml(const QString &value) {
    if (!contentField_) return;
    const QSignalBlocker blocker(contentField_);
    contentField_->setHtml(value);
}

QString NoteEditor::plainText() const {
    return contentField_ ? contentField_->toPlainText() : QString();
}

void NoteEditor::setPlainText(const QString &value) {
    if (!contentField_) return;
    const QSignalBlocker blocker(contentField_);
    contentField_->setPlainText(value);
}

QString NoteEditor::displayTitle() const {
    const QString written = title().trimmed();
    if (!written.isEmpty()) return written;
    const QString firstLine = plainText().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (!firstLine.isEmpty()) return firstLine.left(60);
    return L(QStringLiteral("Neue Notiz"), QStringLiteral("New note"));
}

void NoteEditor::focusContent() {
    if (contentField_) contentField_->setFocus();
}

} // namespace yobro::spike
