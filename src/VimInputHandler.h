#ifndef VIMINPUTHANDLER_H
#define VIMINPUTHANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>
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
        VisualLine,
        VisualBlock
    };
    Q_ENUM(Mode)

    explicit VimInputHandler(QsciScintilla* editor, QObject* parent = nullptr);
    ~VimInputHandler() override;

    void setEnabled(bool enabled);
    bool isEnabled() const;
    Mode mode() const;
    // A true result means the command completed or a nonmodal substitution
    // confirmation started; it does not imply that text was already changed.
    bool executeCommand(const QString& command);
    bool loadConfig(const QString& path);

signals:
    void modeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handleKeyPress(QKeyEvent* event);
    bool handleKeyPressImpl(QKeyEvent* event);
    void beginTemporaryNormal();
    void finishTemporaryNormal(const QString& key = QString(), bool history = false,
                               const QString& pending = QString());
    void finishChangeSequence(const QString& key, bool history);
    struct InsertPause {
        int depth, line, position, column;
        bool replace, atEnd;
    };
    QVector<InsertPause> m_insertPauses;
    int m_keyDispatchDepth = 0;
    bool handleNormalKey(QKeyEvent* event);
    bool handleVisualKey(QKeyEvent* event);
    bool handlePendingKey(QKeyEvent* event);
    bool handleControlKey(QKeyEvent* event);
    bool handleCustomMapping(QKeyEvent* event);
    bool executeCustomMapping(const QString& mapping);
    bool isCustomMappingPrefix(const QString& mapping) const;
    void flushInsertMappingPrefix();
    void resetInsertCompletion();
    void completeInsertWord(bool forward);
    void forwardInsertKey(QKeyEvent* event);
    void backspaceInsert(bool wholeLine);
    void indentInsert(bool increase);
    void foldCommand(const QString& command, int count);

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
    void setRegister(const QString& text, bool linewise, bool yank = false, bool blockwise = false);
    QString registerText(const QString& name, bool& linewise, bool& blockwise) const;
    QString blockRow(int line, int left, int right, const QString& value, bool replace);
    void insertBlock(int line, int column, const QStringList& rows, int count);
    void paste(bool before, int count);
    void pasteVisual(bool preserveRegisters, int count);

    void enterVisualMode(bool linewise);
    void updateVisualSelection();
    void finishBlockOperator(const QString& command);
    void finishBlockInsert();
    bool m_blockInsert = false;
    int m_blockFirst = 0, m_blockLast = 0, m_blockColumn = 0, m_blockStart = 0;
    QString m_blockBefore;
    bool m_registerBlock = false;
    QVector<int> m_changes;
    int m_changeIndex = 0;
    QVector<int> m_jumps;
    int m_jumpIndex = -1;
    void finishVisualOperator(const QString& command);

    void promptSearch(bool forward);
    void repeatSearch(bool reverse);
    QVector<QPair<int, int>> searchMatches(const QString& pattern) const;
    bool moveToSearch(const QString& pattern, bool forward, int start, int count);
    void paintSearch(const QString& pattern);
    void previewSearch();
    void restoreSearchOrigin();
    QVector<int> m_searchClosedFolds;
    void finishSearch(bool accept, bool restore = true, bool focus = true);
    QLineEdit* m_searchPrompt = nullptr;
    QStringList m_searchHistory;
    QString m_searchDraft;
    bool m_searchActive = false, m_searchForward = true, m_searchHighlight = false;
    int m_searchOrigin = 0, m_searchAnchor = 0, m_searchCount = 1;
    int m_searchTop = 0, m_searchX = 0, m_searchHistoryIndex = 0;
    int m_searchIndicator = -1;

    struct Stroke
    {
        int key;
        Qt::KeyboardModifiers modifiers;
        QString text;
        // Dot repeat reuses accepted completion edits; recorded macros keep raw keys.
        int completionDeleteBytes = -1;
        QString completionText;
    };
    using Strokes = QVector<Stroke>;
    bool processStroke(QKeyEvent* event);
    void promptCommand();
    bool executeCommandImpl(const QString& command);
    void recordExChange(int position);
    bool m_globalActive = false, m_exValidateOnly = false;
    bool m_substituteChangeRecorded = false;
    struct SubstituteMatch { int first, last; QString replacement; };
    bool beginSubstituteConfirmation(const QVector<SubstituteMatch>& matches);
    bool handleSubstituteConfirmation(QKeyEvent* event);
    bool acceptSubstituteMatch();
    void showSubstituteMatch();
    void finishSubstituteConfirmation(bool restoreFocus = true);
    QVector<SubstituteMatch> m_substituteMatches;
    QByteArray m_substituteExpected;
    QLineEdit* m_substitutePrompt = nullptr;
    int m_substituteIndex = 0, m_substituteOffset = 0;
    bool m_substituteActive = false, m_substituteChanging = false, m_substituteUndoOpen = false;
    void playMapping(const QString& mapping);
    QMap<QString, QString> m_userMappings;
    QString m_leader = ",";
    QLineEdit* m_commandLine = nullptr;
    void replay(const Strokes& keys, int count);
    bool extendedNormal(const QString& key);
    bool extendedPending(const QString& key);
    bool handleGMotion(const QString& key);
    bool findCharacter(const QString& command, const QString& target, int count);
    bool applyFindMotion(const QString& command, const QString& target, int count, bool repeat,
                         const QString& operation = QString());
    bool repeatFindMotion(bool reverse, int count, const QString& operation = QString());
    void transformRange(int first, int last, const QString& operation);
    void indentLines(int first, int last, const QString& operation);
    bool m_forwarding = false;
    int m_replayDepth = 0;
    int m_replayBudget = 0;
    bool m_groupOpen = false;
    QString m_changeBefore;
    Strokes m_sequence, m_lastChange;
    QMap<QString, Strokes> m_macros;
    QString m_recording, m_lastMacro;
    QMap<QString, QPair<QString, bool>> m_registers;
    QMap<QString, bool> m_blockRegisters;
    QString m_selectedRegister;
    bool m_insertRegisterPending = false;
    QMap<QString, int> m_marks;
    QByteArray m_trackedText;
    QString m_findCommand, m_findTarget;
    int m_savedAnchor = 0, m_savedCaret = 0;
    Mode m_savedVisualMode = Mode::Visual;
    bool m_replace = false;
    struct ReplaceEdit { int start; QByteArray original, inserted; };
    QVector<ReplaceEdit> m_replaceEdits;
    int m_insertBackspaceStart = 0;
    bool m_insertTextEntered = false;
    int m_insertRepeat = 1, m_insertStart = 0;
    QString m_insertBefore;
    bool m_repeatNewline = false;

    QStringList m_completionCandidates;
    int m_completionStart = 0, m_completionEnd = 0, m_completionIndex = 0;
    bool m_completionChanging = false;
    bool m_completionForward = true;

    QsciScintilla* m_editor;
    bool m_enabled;
    Mode m_mode;
    int m_count;
    QString m_pendingCommand;
    int m_pendingCount;
    int m_visualAnchor;
    int m_visualCaret;
    bool m_visualTagSelected = false;
    QString m_registerText;
    bool m_registerLinewise;
    bool m_haveSubstitute = false;
    QString m_substitutePattern, m_substituteReplacement, m_substituteFlags;
    QString m_lastSearch;
    bool m_lastSearchForward;
    QString m_mappingPrefix;
    QTimer* m_mappingTimer;
};

#endif
