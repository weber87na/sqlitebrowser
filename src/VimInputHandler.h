#ifndef VIMINPUTHANDLER_H
#define VIMINPUTHANDLER_H

#include <QObject>
#include <QString>

class QEvent;
class QKeyEvent;
class QsciScintilla;

/**
 * @brief Adds a small, self-contained Vim emulation layer to QScintilla.
 *
 * The handler deliberately implements the editing commands people use most
 * often instead of trying to emulate every Vim feature. When disabled it does
 * not consume any events, so the editor behaves exactly like a normal
 * QScintilla widget.
 */
class VimInputHandler : public QObject
{
    Q_OBJECT

public:
    enum class Mode
    {
        Normal,
        Insert,
        Visual,
        VisualLine
    };
    Q_ENUM(Mode)

    explicit VimInputHandler(QsciScintilla* editor, QObject* parent = nullptr);
    ~VimInputHandler() override;

    void setEnabled(bool enabled);
    bool isEnabled() const;
    Mode mode() const;

signals:
    void modeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handleKeyPress(QKeyEvent* event);
    bool handleNormalKey(QKeyEvent* event);
    bool handleVisualKey(QKeyEvent* event);
    bool handlePendingKey(QKeyEvent* event);
    bool handleControlKey(QKeyEvent* event);

    void setMode(Mode mode);
    void resetPendingCommand();
    int takeCount();
    int currentPosition() const;
    int documentLength() const;
    int currentLine() const;
    int positionFromLine(int line) const;
    int lineEndPosition(int line) const;
    int positionAfter(int position) const;
    int positionBefore(int position) const;
    void setPosition(int position);
    void setSelection(int start, int end);
    void clampNormalCaret();

    bool move(const QString& command, int count);
    bool applyOperatorMotion(const QString& command, int count);
    void applyCharacterOperator(int start, int end);
    void applyLineOperator(int firstLine, int lastLine);
    void deleteCharacter(int count, bool enterInsertMode = false);
    void replaceCharacter(const QString& replacement, int count);
    void joinLines(int count);
    void toggleCase(int count);

    QString linesText(int firstLine, int lastLine) const;
    QString endOfLine() const;
    void setRegister(const QString& text, bool linewise);
    void paste(bool before, int count);

    void enterVisualMode(bool linewise);
    void updateVisualSelection();
    void finishVisualOperator(const QString& command);

    void promptSearch(bool forward);
    void repeatSearch(bool reverse);

    QsciScintilla* m_editor;
    bool m_enabled;
    Mode m_mode;
    int m_count;
    QString m_pendingCommand;
    int m_pendingCount;
    int m_visualAnchor;
    int m_visualCaret;
    QString m_registerText;
    bool m_registerLinewise;
    QString m_lastSearch;
    bool m_lastSearchForward;
};

#endif
