#ifndef VIMINPUTHANDLER_H
#define VIMINPUTHANDLER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QVector>

class QEvent;
class QKeyEvent;
class QsciScintilla;
class QTimer;
class QLineEdit;

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
    bool executeCommand(const QString& command);
    bool loadConfig(const QString& path);

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
    bool handleCustomMapping(QKeyEvent* event);
    bool executeCustomMapping(const QString& mapping);
    bool isCustomMappingPrefix(const QString& mapping) const;
    void flushInsertMappingPrefix();

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
    int characterClassAt(int position) const;
    int nextWordEndPosition(int position) const;
    void setPosition(int position);
    void setSelection(int start, int end);
    void clampNormalCaret();

    bool move(const QString& command, int count);
    bool textObjectRange(const QString& object, bool around, int count, int& first, int& last) const;
    bool handleSurroundKey(const QString& key);
    void finishSurround(const QString& delimiter, bool remove = false);
    int m_surroundStart = 0;
    int m_surroundEnd = 0;
    bool applyTextObject(const QString& object, bool around, int count);
    bool applyOperatorMotion(const QString& command, int count);
    void applyCharacterOperator(int start, int end);
    void applyLineOperator(int firstLine, int lastLine);
    void deleteCharacter(int count, bool enterInsertMode = false);
    void replaceCharacter(const QString& replacement, int count);
    void joinLines(int count, bool raw = false);
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

    struct Stroke { int key; Qt::KeyboardModifiers modifiers; QString text; };
    using Strokes = QVector<Stroke>;
    bool processStroke(QKeyEvent* event);
    void promptCommand();
    void playMapping(const QString& mapping);
    QMap<QString, QString> m_userMappings;
    QString m_leader = ",";
    QLineEdit* m_commandLine = nullptr;
    void replay(const Strokes& keys, int count);
    bool extendedNormal(const QString& key);
    bool extendedPending(const QString& key);
    bool findCharacter(const QString& command, const QString& target, int count);
    void transformRange(int first, int last, const QString& operation);
    void indentLines(int first, int last, const QString& operation);
    bool m_forwarding = false;
    int m_replayDepth = 0;
    bool m_groupOpen = false;
    QString m_changeBefore;
    Strokes m_sequence, m_lastChange;
    QMap<QString, Strokes> m_macros;
    QString m_recording, m_lastMacro;
    QMap<QString, QPair<QString, bool>> m_registers;
    QString m_selectedRegister;
    QMap<QString, int> m_marks;
    QString m_findCommand, m_findTarget;
    int m_savedAnchor = 0, m_savedCaret = 0;
    Mode m_savedVisualMode = Mode::Visual;
    bool m_replace = false;

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
    QString m_mappingPrefix;
    QTimer* m_mappingTimer;
};

#endif
