#pragma once

#include <QString>
#include <QWidget>

class QLineEdit;
class QTextEdit;

namespace yobro::spike {

/// A note that lives in a tab, like the WebKit build's note tabs.
///
/// One difference is worth knowing: the WebKit build stores rich text as RTF,
/// because that is what its text view produces. Qt's editor works with HTML, so
/// notes are stored as HTML here. The two apps keep separate profiles, so no
/// note ever has to be read by both.
class NoteEditor : public QWidget {
    Q_OBJECT

public:
    explicit NoteEditor(QWidget *parent = nullptr);

    [[nodiscard]] QString title() const;
    void setTitle(const QString &value);

    /// The formatted content.
    [[nodiscard]] QString html() const;
    void setHtml(const QString &value);

    /// The content without formatting, for search and for the sidebar.
    [[nodiscard]] QString plainText() const;
    /// Replaces the content with unformatted text.
    ///
    /// Used for text the assistant produced: it is data, not markup, so any
    /// angle bracket in it must show up as an angle bracket and must not be
    /// able to inject a link or an image into the note.
    void setPlainText(const QString &value);

    /// The title if it has one, otherwise the first line of the content, and
    /// finally a placeholder. Never empty.
    [[nodiscard]] QString displayTitle() const;

    void focusContent();

Q_SIGNALS:
    /// Emitted whenever the title or the content changed, so the owner can save.
    void changed();

private:
    QLineEdit *titleField_ = nullptr;
    QTextEdit *contentField_ = nullptr;
};

} // namespace yobro::spike
