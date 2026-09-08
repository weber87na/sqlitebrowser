#include "VimInputHandler.h"

#include <Qsci/qsciscintilla.h>

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStringList>
#include <QTimer>
#include <QRegularExpression>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QScopedValueRollback>
#include <QSet>

#include <algorithm>
#include <cctype>
#include <limits>

namespace
{
constexpr int MaximumCount = 9999;

QString commandKey(const QKeyEvent* event)
{
    if(event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        return "\r";

    // QKeyEvent::text() can remain lower-case for synthetic events and on some
    // keyboard layouts.  Letter commands must still distinguish v from V, etc.
    if(event->modifiers().testFlag(Qt::ShiftModifier) &&
       event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
    {
        return QString(QChar::fromLatin1(static_cast<char>(event->key())));
    }

    return event->text();
}

bool isRegisterName(const QString& name)
{
    return name.size() == 1 &&
        QString("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"-+*_").contains(name);
}

bool tagObjectRange(const QByteArray& text, int caret, bool around, int count,
                    bool expanding, int selectionFirst, int selectionLast, int& first, int& last)
{
    struct Tag { QByteArray name; int first; int end; };
    struct Pair { int first; int innerFirst; int innerLast; int last; };
    QVector<Tag> stack;
    QVector<Pair> pairs;
    const int length = text.size();
    const auto whitespace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
    };
    const auto nameStart = [](char c) {
        const unsigned char value = static_cast<unsigned char>(c);
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               c == '_' || c == ':' || value >= 0x80;
    };
    const auto namePart = [&nameStart](char c) {
        return nameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
    };
    const auto foldedName = [](const QByteArray& name) {
        // Vim's tag objects ignore case even for XML. Keep the original bytes
        // untouched; folding is used only to match names and HTML void tags.
        return QString::fromUtf8(name).toCaseFolded().toUtf8();
    };
    const QSet<QByteArray> voidTags = {"area", "base", "br", "col", "embed", "hr", "img",
                                     "input", "link", "meta", "param", "source", "track", "wbr"};
    const QSet<QByteArray> rawTags = {"script", "style", "textarea", "title"};
    int p = 0;
    while(p < length)
    {
        if(text.at(p) != '<') { ++p; continue; }
        if(!stack.isEmpty() && rawTags.contains(stack.last().name))
        {
            // HTML raw/RCDATA elements only recognize their own closing tag.
            // In particular, '<' in JavaScript and CSS isn't a nested element.
            int nameEnd = p + 2;
            if(p + 1 >= length || text.at(p + 1) != '/') { ++p; continue; }
            while(nameEnd < length && namePart(text.at(nameEnd))) ++nameEnd;
            if(foldedName(text.mid(p + 2, nameEnd - p - 2)) != stack.last().name ||
               (nameEnd < length && text.at(nameEnd) != '>' && !whitespace(text.at(nameEnd))))
            { ++p; continue; }
        }
        if(text.mid(p, 4) == "<!--")
        {
            const int end = text.indexOf("-->", p + 4);
            if(end < 0) return false;
            p = end + 3;
            continue;
        }
        if(text.mid(p, 9) == "<![CDATA[")
        {
            const int end = text.indexOf("]]>", p + 9);
            if(end < 0) return false;
            p = end + 3;
            continue;
        }
        if(text.mid(p, 2) == "<?")
        {
            // XML PIs end at the first ?>; quotes in their contents have no
            // syntactic meaning (unlike quoted values in start tags).
            int targetEnd = p + 2;
            if(targetEnd >= length || !nameStart(text.at(targetEnd))) return false;
            while(targetEnd < length && namePart(text.at(targetEnd))) ++targetEnd;
            if(targetEnd < length && !whitespace(text.at(targetEnd)) && text.mid(targetEnd, 2) != "?>")
                return false;
            const int end = text.indexOf("?>", targetEnd);
            if(end < 0) return false;
            p = end + 2;
            continue;
        }
        if(text.mid(p, 2) == "<!")
        {
            int q = p + 2;
            if(q >= length || !nameStart(text.at(q))) return false;
            int brackets = 0;
            char quote = 0;
            bool closed = false;
            for(; q < length; ++q)
            {
                const char c = text.at(q);
                if(quote) { if(c == quote) quote = 0; continue; }
                if(c == '\'' || c == '"') { quote = c; continue; }
                if(text.mid(q, 4) == "<!--")
                {
                    const int end = text.indexOf("-->", q + 4);
                    if(end < 0) return false;
                    q = end + 2;
                    continue;
                }
                if(c == '[') ++brackets;
                if(c == ']' && --brackets < 0) return false;
                if(c == '>' && brackets == 0)
                {
                    p = q + 1;
                    closed = true;
                    break;
                }
            }
            if(!closed) return false;
            continue;
        }
        const int tagFirst = p;
        int q = p + 1;
        const bool closing = q < length && text.at(q) == '/';
        if(closing) ++q;
        if(q >= length || !nameStart(text.at(q)))
        {
            // A literal comparison such as 'a < b' isn't markup. A malformed
            // closing tag is ambiguous and must not cause an edit.
            if(closing) return false;
            ++p;
            continue;
        }
        const int nameFirst = q++;
        while(q < length && namePart(text.at(q))) ++q;
        const QByteArray name = foldedName(text.mid(nameFirst, q - nameFirst));
        bool selfClosing = false;
        bool closed = false;
        QSet<QByteArray> attributes;
        while(q < length)
        {
            const int beforeWhitespace = q;
            while(q < length && whitespace(text.at(q))) ++q;
            if(q >= length) return false;
            if(text.at(q) == '>') { ++q; closed = true; break; }
            if(!closing && text.at(q) == '/' && q + 1 < length && text.at(q + 1) == '>')
            { q += 2; selfClosing = true; closed = true; break; }
            if(closing || q == beforeWhitespace || !nameStart(text.at(q))) return false;
            const int attributeFirst = q++;
            while(q < length && namePart(text.at(q))) ++q;
            const QByteArray attribute = foldedName(text.mid(attributeFirst, q - attributeFirst));
            if(attributes.contains(attribute)) return false;
            attributes.insert(attribute);
            const int attributeEnd = q;
            while(q < length && whitespace(text.at(q))) ++q;
            if(q >= length) return false;
            if(text.at(q) != '=')
            {
                q = attributeEnd; // A boolean HTML attribute; preserve its following whitespace.
                continue;
            }
            ++q;
            while(q < length && whitespace(text.at(q))) ++q;
            if(q >= length) return false;
            if(text.at(q) == '\'' || text.at(q) == '"')
            {
                const char quote = text.at(q++);
                while(q < length && text.at(q) != quote) ++q;
                if(q == length) return false;
                ++q;
            }
            else
            {
                const int valueFirst = q;
                while(q < length && !whitespace(text.at(q)) && text.at(q) != '>' &&
                      !(text.at(q) == '/' && q + 1 < length && text.at(q + 1) == '>'))
                {
                    if(text.at(q) == '<' || text.at(q) == '=' || text.at(q) == '"' ||
                       text.at(q) == '\'' || text.at(q) == '`') return false;
                    ++q;
                }
                if(q == valueFirst) return false;
            }
        }
        if(!closed) return false;
        if(closing)
        {
            // No browser-style repair: mismatched, missing, or stray closing
            // tags invalidate the scan, including matches completed earlier.
            if(stack.isEmpty() || stack.last().name != name) return false;
            const Tag open = stack.takeLast();
            pairs.append({open.first, open.end, tagFirst, q});
        }
        else if(!selfClosing && !voidTags.contains(name))
            stack.append({name, tagFirst, q});
        p = q;
    }
    if(!stack.isEmpty()) return false;

    // A stack scan completes nested pairs before their parents, so enclosing
    // matches are naturally ordered from the innermost to the outermost pair.
    for(const Pair& pair : pairs)
    {
        if(caret < pair.first || caret >= pair.last) continue;
        int begin = around ? pair.first : pair.innerFirst;
        int end = around ? pair.last : pair.innerLast;
        if(expanding)
        {
            if(selectionFirst < pair.first || selectionLast > pair.last) continue;
            if(!around && begin >= selectionFirst && end <= selectionLast)
            {
                begin = pair.first;
                end = pair.last;
            }
            if(begin > selectionFirst || end < selectionLast ||
               (begin == selectionFirst && end == selectionLast)) continue;
        }
        if(--count == 0)
        {
            first = begin;
            last = end;
            return true;
        }
    }
    return false;
}

bool isMotionOperator(const QString& operation)
{
    return operation == "d" || operation == "c" || operation == "y" || operation == "gu" ||
        operation == "gU" || operation == "g~" || operation == ">" || operation == "<" || operation == "=";
}
}

VimInputHandler::VimInputHandler(QsciScintilla* editor, QObject* parent) :
    QObject(parent),
    m_editor(editor),
    m_enabled(false),
    m_mode(Mode::Insert),
    m_count(0),
    m_pendingCount(1),
    m_visualAnchor(0),
    m_visualCaret(0),
    m_registerLinewise(false),
    m_lastSearchForward(true),
    m_mappingTimer(new QTimer(this))
{
    Q_ASSERT(m_editor);
    // When owned by the editor, QObject deletes this handler after Scintilla's
    // derived destructor has run. Never send Scintilla messages at that point.
    connect(m_editor, &QObject::destroyed, this, [this]() {
        m_editor = nullptr;
        m_enabled = false;
        m_searchActive = false;
        m_substituteActive = false;
    });
    m_editor->installEventFilter(this);
    m_mappingTimer->setSingleShot(true);
    m_mappingTimer->setInterval(700);
    connect(m_mappingTimer, &QTimer::timeout, this, &VimInputHandler::flushInsertMappingPrefix);
    QString config = qEnvironmentVariable("DB4S_VIM_CONFIG");
    if(config.isEmpty()) config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/vim.json";
    loadConfig(config);
    m_trackedText = m_editor->text().toUtf8();
    // Cancel before unrelated edits so cached match positions are never used
    // on new text. Host actions that already opened a nested undo group retain
    // that group; ordinary edits begin after our confirmation group is closed.
    connect(m_editor, &QsciScintillaBase::SCN_MODIFIED, this,
        [this](int, int modification, const char*, int, int, int, int, int, int, int) {
            if(m_substituteActive && !m_substituteChanging &&
               (modification & (QsciScintillaBase::SC_MOD_BEFOREINSERT | QsciScintillaBase::SC_MOD_BEFOREDELETE)))
                finishSubstituteConfirmation(false);
        });
    connect(m_editor, &QsciScintilla::textChanged, this, [this]() {
        if(m_searchActive) finishSearch(false, false, false);
        if(m_searchHighlight) paintSearch(m_lastSearch);
        if(m_substituteActive && !m_substituteChanging) finishSubstituteConfirmation(false);
        if(!m_completionChanging) resetInsertCompletion();
        const QByteArray next = m_editor->text().toUtf8();
        int first = 0, oldEnd = m_trackedText.size(), newEnd = next.size();
        while(first < oldEnd && first < newEnd && m_trackedText.at(first) == next.at(first)) ++first;
        while(oldEnd > first && newEnd > first && m_trackedText.at(oldEnd-1) == next.at(newEnd-1)) { --oldEnd; --newEnd; }
        auto adjust = [&](int& position) { if(position >= oldEnd) position += newEnd-oldEnd; else if(position > first) position = first; };
        for(auto it = m_marks.begin(); it != m_marks.end(); ++it) adjust(it.value());
        for(int& position : m_jumps) adjust(position);
        for(int& position : m_changes) adjust(position);
        if(m_mode == Mode::Insert && next != m_trackedText)
            m_insertTextEntered = true;
        m_trackedText = next;
    });
    connect(m_editor, &QsciScintilla::cursorPositionChanged, this, [this]() {
        if(!m_completionChanging) resetInsertCompletion();
    });
    connect(m_editor, &QsciScintilla::selectionChanged, this, [this]() {
        if(!m_completionChanging) resetInsertCompletion();
    });
}

VimInputHandler::~VimInputHandler()
{
    if(m_editor)
    {
        finishSearch(false, false, false);
        paintSearch(QString());
        finishSubstituteConfirmation(false);
        if(m_groupOpen) m_editor->endUndoAction();
        m_editor->removeEventFilter(this);
    }
}

void VimInputHandler::setEnabled(bool enabled)
{
    if(m_enabled == enabled)
        return;

    finishSearch(false);
    paintSearch(QString());
    m_searchHighlight = false;
    finishSubstituteConfirmation(false);
    if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    m_sequence.clear();
    m_insertPauses.clear();
    m_enabled = enabled;
    m_insertRegisterPending = false;
    m_selectedRegister.clear();
    resetPendingCommand();
    m_mappingPrefix.clear();
    m_mappingTimer->stop();
    setMode(enabled ? Mode::Normal : Mode::Insert);
    if(!enabled)
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION, currentPosition());
}

bool VimInputHandler::isEnabled() const
{
    return m_enabled;
}

VimInputHandler::Mode VimInputHandler::mode() const
{
    return m_mode;
}

bool VimInputHandler::eventFilter(QObject* watched, QEvent* event)
{
    // QWidget teardown can send FocusOut before QObject::destroyed, after the
    // Scintilla subobject is already gone. Its runtime metaobject reveals this.
    if(m_editor && !qobject_cast<QsciScintilla*>(static_cast<QObject*>(m_editor)))
    {
        m_editor = nullptr;
        m_enabled = false;
        m_searchActive = false;
        m_substituteActive = false;
    }
    if(m_searchActive)
    {
        if(watched == m_editor && event->type() == QEvent::KeyPress)
        {
            if(!m_forwarding)
            {
                QScopedValueRollback<bool> forwarding(m_forwarding, true);
                QCoreApplication::sendEvent(m_searchPrompt, event);
            }
            return true;
        }
        if(watched == m_searchPrompt && event->type() == QEvent::ShortcutOverride)
        { event->accept(); return true; }
        if(watched == m_searchPrompt && event->type() == QEvent::FocusOut)
            finishSearch(false, true, false);
        if(watched == m_editor && (event->type() == QEvent::MouseButtonPress ||
           event->type() == QEvent::MouseButtonDblClick))
            finishSearch(false, true, false);
        if(watched == m_editor && event->type() == QEvent::Resize)
            m_searchPrompt->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
        if(watched == m_searchPrompt && event->type() == QEvent::KeyPress)
        {
            auto* key = static_cast<QKeyEvent*>(event);
            if(!m_recording.isEmpty() && m_replayDepth == 0 && !m_forwarding)
                m_macros[m_recording].append({key->key(), key->modifiers(), key->text()});
            if(key->key() == Qt::Key_Escape || (key->key() == Qt::Key_BracketLeft &&
               key->modifiers() == Qt::ControlModifier))
            { finishSearch(false); return true; }
            if(key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            { finishSearch(true); return true; }
            if(key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)
            {
                if(m_searchHistoryIndex == m_searchHistory.size()) m_searchDraft = m_searchPrompt->text();
                m_searchHistoryIndex = std::max(0, std::min(m_searchHistory.size(),
                    m_searchHistoryIndex + (key->key() == Qt::Key_Up ? -1 : 1)));
                m_searchPrompt->setText(m_searchHistoryIndex == m_searchHistory.size() ?
                    m_searchDraft : m_searchHistory.at(m_searchHistoryIndex));
                return true;
            }
        }
    }
    if(m_substituteActive && (watched == m_editor || watched == m_substitutePrompt))
    {
        if(event->type() == QEvent::ShortcutOverride)
        { event->accept(); return true; }
        if(event->type() == QEvent::KeyPress)
            return handleSubstituteConfirmation(static_cast<QKeyEvent*>(event));
        if(watched == m_editor && (event->type() == QEvent::MouseButtonPress ||
           event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::InputMethod))
            finishSubstituteConfirmation(false);
        if(watched == m_editor && event->type() == QEvent::Resize)
            m_substitutePrompt->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
    }
    if(watched == m_commandLine && !m_insertPauses.isEmpty() && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if(key == Qt::Key_Return || key == Qt::Key_Enter)
        {
            // QLineEdit can propagate Return after emitting returnPressed. Once
            // CTRL-O resumes Insert, that same event must not insert a newline.
            QMetaObject::invokeMethod(m_commandLine, "returnPressed", Qt::DirectConnection);
            return true;
        }
    }
    if(m_forwarding || !m_enabled || watched != m_editor)
        return QObject::eventFilter(watched, event);

    if(event->type() == QEvent::FocusOut || event->type() == QEvent::InputMethod)
    {
        resetInsertCompletion();
        flushInsertMappingPrefix();
    }
    if(event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);

    auto* key = static_cast<QKeyEvent*>(event);
    // Modifier key presses are not Vim commands and must not consume a count.
    if(key->key() == Qt::Key_Control || key->key() == Qt::Key_Shift ||
       key->key() == Qt::Key_Alt || key->key() == Qt::Key_Meta || key->key() == Qt::Key_AltGr)
        return false;
    if(m_mode == Mode::Insert && (key->text().isEmpty() ||
       key->modifiers().testFlag(Qt::ControlModifier)))
        flushInsertMappingPrefix();

    return processStroke(static_cast<QKeyEvent*>(event));
}

bool VimInputHandler::handleKeyPress(QKeyEvent* event)
{
    ++m_keyDispatchDepth;
    const int pauses = m_insertPauses.size();
    const QString pending = m_pendingCommand;
    const QString key = commandKey(event);
    const bool history = m_mode == Mode::Normal && pending.isEmpty() &&
        (key == "u" || (event->key() == Qt::Key_R && event->modifiers().testFlag(Qt::ControlModifier)));
    if(m_replayDepth > 0)
    {
        if(history && m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
        if(!history && !m_groupOpen) { m_editor->beginUndoAction(); m_groupOpen = true; }
    }
    bool handled = handleKeyPressImpl(event);
    if(!handled && pauses > 0 && m_insertPauses.size() == pauses &&
       m_insertPauses.last().depth == m_keyDispatchDepth)
    {
        m_forwarding = true;
        QCoreApplication::sendEvent(m_editor, event);
        m_forwarding = false;
        handled = true;
    }
    // A mapping or macro can run several strokes inside one Normal command.
    // Only the dispatch level which entered CTRL-O may complete that pause.
    if(pauses > 0 && m_insertPauses.size() == pauses &&
       m_insertPauses.last().depth == m_keyDispatchDepth &&
       (m_mode == Mode::Normal || m_mode == Mode::Insert) &&
       m_pendingCommand.isEmpty() && m_mappingPrefix.isEmpty() && m_count == 0 &&
       pending != "\"" && !m_substituteActive && !m_searchActive && (!m_commandLine || m_commandLine->isHidden()))
        finishTemporaryNormal(key, history, pending);
    --m_keyDispatchDepth;
    return handled;
}

void VimInputHandler::beginTemporaryNormal()
{
    flushInsertMappingPrefix();
    finishBlockInsert();
    // CTRL-O ends the current insertion's undo block and cancels its count.
    m_insertRepeat = 1;
    m_repeatNewline = false;
    if(m_replayDepth == 0)
    {
        if(!m_sequence.isEmpty()) m_sequence.removeLast();
        m_sequence.append({Qt::Key_Escape, Qt::NoModifier, QString()});
        finishChangeSequence(QString(), false);
    }
    else if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    const int caret = currentPosition();
    m_insertPauses.append({m_keyDispatchDepth, currentLine(), caret,
        int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, caret)),
        m_replace, caret == lineEndPosition(currentLine())});
    resetPendingCommand();
    m_selectedRegister.clear();
    m_replace = false;
    m_editor->setOverwriteMode(false);
    // Do not finalize counted insertion or shift the cursor like Escape does.
    m_mode = Mode::Normal;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETCARETSTYLE, QsciScintillaBase::CARETSTYLE_BLOCK);
    clampNormalCaret();
    emit modeChanged();
    m_changeBefore = m_editor->text();
}

void VimInputHandler::finishTemporaryNormal(const QString& key, bool history, const QString& pending)
{
    if(m_insertPauses.isEmpty()) return;
    const InsertPause pause = m_insertPauses.takeLast();
    m_selectedRegister.clear();
    if(m_mode == Mode::Insert)
    {
        // i/a/c/o/R explicitly choose the next insertion mode; they do not nest.
        return;
    }
    if(m_mode != Mode::Normal) return;
    const bool changed = m_editor->text() != m_changeBefore;
    const int lineEnd = lineEndPosition(currentLine());
    if((pending == "y" && (key == "y" || key == "j" || key == "k" || key == "+" ||
        key == "-" || key == "_" || key == "\r" || key == "G")) || (pending == "yg" && key == "g"))
    {
        // A linewise yank preserves the original column when insertion resumes.
        setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                                currentLine(), pause.column)));
    }
    else if(pending.isEmpty() && (key == "j" || key == "k") && pause.atEnd)
    {
        // The original insertion column can be one past the last character.
        setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                                currentLine(), pause.column)));
    }
    else if((pending.isEmpty() && key == "$") ||
            (currentLine() == pause.line && currentPosition() == positionBefore(lineEnd) &&
             (pause.atEnd || (changed && pause.position >= lineEnd))))
        setPosition(lineEnd);
    if(m_replayDepth == 0)
        finishChangeSequence(key, history);
    else if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    setMode(Mode::Insert);
    m_replace = pause.replace;
    m_editor->setOverwriteMode(m_replace);
    if(m_replayDepth == 0)
    {
        // Dot repeats the insertion after CTRL-O, as a new Insert/Replace run.
        m_sequence.append({m_replace ? Qt::Key_R : Qt::Key_I,
                           m_replace ? Qt::ShiftModifier : Qt::NoModifier,
                           m_replace ? QString("R") : QString("i")});
        m_changeBefore = m_editor->text();
    }
}

bool VimInputHandler::handleKeyPressImpl(QKeyEvent* event)
{
    if(m_searchActive)
    { QCoreApplication::sendEvent(m_searchPrompt, event); return true; }
    const bool completionKey = m_mode == Mode::Insert && !m_insertRegisterPending && event->modifiers() == Qt::ControlModifier &&
        (event->key() == Qt::Key_N || event->key() == Qt::Key_P);
    if(!completionKey) resetInsertCompletion();
    if(completionKey)
    {
        flushInsertMappingPrefix();
        completeInsertWord(event->key() == Qt::Key_N);
        return true;
    }

    const bool escape = event->key() == Qt::Key_Escape ||
        (event->key() == Qt::Key_BracketLeft && event->modifiers().testFlag(Qt::ControlModifier));

    if(m_mode == Mode::Insert && m_insertRegisterPending)
    {
        m_insertRegisterPending = false;
        // Escape cancels only the register prompt; insertion can continue.
        if(escape) return true;
        const QString name = commandKey(event);
        if(!isRegisterName(name) || event->modifiers().testFlag(Qt::ControlModifier) ||
           event->modifiers().testFlag(Qt::AltModifier) || event->modifiers().testFlag(Qt::MetaModifier) ||
           m_editor->isReadOnly()) return true;
        bool linewise = false, blockwise = false;
        QString value = registerText(name, linewise, blockwise);
        // Registers may originate in another editor with different line endings.
        value.replace("\r\n", "\n");
        value.replace('\r', '\n');
        value.replace("\n", endOfLine());
        if(!value.isEmpty()) m_editor->replaceSelectedText(value);
        return true;
    }

    if(m_mode == Mode::Insert && event->key() == Qt::Key_O &&
       event->modifiers() == Qt::ControlModifier)
    {
        beginTemporaryNormal();
        return true;
    }

    if(escape)
    {
        finishBlockInsert();
        if(m_mode == Mode::Insert)
            flushInsertMappingPrefix();
        else
            m_mappingPrefix.clear();
        m_replace = false;
        m_editor->setOverwriteMode(false);

        if(m_mode == Mode::Insert)
        {
            if(currentPosition() > positionFromLine(currentLine()))
                setPosition(positionBefore(currentPosition()));
            setMode(Mode::Normal);
            clampNormalCaret();
        }
        else
        {
            setPosition(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock ? m_visualCaret : currentPosition());
            setMode(Mode::Normal);
        }
        resetPendingCommand();
        m_selectedRegister.clear();
        return true;
    }

    if(m_mode == Mode::Insert && event->modifiers() == Qt::ControlModifier &&
       (event->key() == Qt::Key_T || event->key() == Qt::Key_D))
    {
        flushInsertMappingPrefix();
        indentInsert(event->key() == Qt::Key_T);
        return true;
    }

    if(m_mode == Mode::Insert && event->key() == Qt::Key_R &&
       event->modifiers().testFlag(Qt::ControlModifier))
    {
        flushInsertMappingPrefix();
        m_insertRegisterPending = true;
        return true;
    }

    if(m_mode == Mode::Insert && event->modifiers().testFlag(Qt::ControlModifier) &&
       (event->key() == Qt::Key_W || event->key() == Qt::Key_H || event->key() == Qt::Key_U))
    {
        flushInsertMappingPrefix();
        if(m_editor->isReadOnly()) return true;
        if(event->key() == Qt::Key_W)
            m_editor->SendScintilla(QsciScintillaBase::SCI_DELWORDLEFT);
        else
            backspaceInsert(event->key() == Qt::Key_U);
        return true;
    }

    if(handleCustomMapping(event))
        return true;

    if(m_mode == Mode::Insert)
    {
        if(event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier)
            backspaceInsert(false);
        else
            forwardInsertKey(event);
        return true;
    }

    if(event->modifiers().testFlag(Qt::AltModifier) || event->modifiers().testFlag(Qt::MetaModifier))
        return false;

    if(event->modifiers().testFlag(Qt::ControlModifier))
        return handleControlKey(event);

    if(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
        return handleVisualKey(event);

    return handleNormalKey(event);
}

void VimInputHandler::indentInsert(bool increase)
{
    if(m_editor->isReadOnly() || m_editor->hasSelectedText()) return;
    const int line = currentLine();
    const int caret = currentPosition();
    const int oldEnd = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEINDENTPOSITION, line));
    int width = m_editor->indentationWidth();
    if(width <= 0) width = m_editor->tabWidth();
    width = std::max(1, width);
    const int indentation = m_editor->indentation(line);
    const int target = increase ? (indentation / width + 1) * width
                               : std::max(0, (indentation - 1) / width * width);
    m_editor->setIndentation(line, target);
    const int newEnd = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEINDENTPOSITION, line));
    const int delta = newEnd - oldEnd;
    setPosition(caret >= oldEnd ? caret + delta : newEnd);
    if(m_insertBackspaceStart >= oldEnd) m_insertBackspaceStart += delta;
    else m_insertBackspaceStart = std::min(m_insertBackspaceStart, newEnd);
    // Existing indentation was edited, so this is no longer a simple counted insertion.
    m_insertRepeat = 1;
    for(ReplaceEdit& edit : m_replaceEdits)
        if(edit.start >= oldEnd) edit.start += delta;
}

void VimInputHandler::foldCommand(const QString& command, int count)
{
    const auto parent = [this](int line) {
        return int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDPARENT, line));
    };
    const auto isHeader = [this](int line) {
        return (m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL, line) &
                QsciScintillaBase::SC_FOLDLEVELHEADERFLAG) != 0;
    };
    const auto expanded = [this](int line) {
        return m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDEXPANDED, line) != 0;
    };
    if(command == "zv")
    {
        m_editor->SendScintilla(QsciScintillaBase::SCI_ENSUREVISIBLE, currentLine());
        return;
    }
    if(command == "zR" || command == "zM")
    {
        m_editor->SendScintilla(QsciScintillaBase::SCI_FOLDALL,
            command == "zR" ? QsciScintillaBase::SC_FOLDACTION_EXPAND : QsciScintillaBase::SC_FOLDACTION_CONTRACT);
    }
    else
    {
        int line = isHeader(currentLine()) ? currentLine() : parent(currentLine());
        if(line < 0) return;
        const bool open = command == "zo" || command == "zO" || (command == "za" && !expanded(line));
        const bool recursive = command == "zO" || command == "zC";
        for(int n = 0; n < count && line >= 0; ++n)
        {
            if(open)
            {
                // Open the outermost closed ancestor first when the caret was hidden.
                int closed = expanded(line) ? -1 : line;
                for(int ancestor = parent(line); ancestor >= 0; ancestor = parent(ancestor))
                    if(!expanded(ancestor)) closed = ancestor;
                if(closed >= 0) line = closed;
            }
            else if(!recursive)
                while(line >= 0 && !expanded(line)) line = parent(line);
            if(line < 0) break;
            const int action = open ? QsciScintillaBase::SC_FOLDACTION_EXPAND : QsciScintillaBase::SC_FOLDACTION_CONTRACT;
            if(recursive) m_editor->SendScintilla(QsciScintillaBase::SCI_FOLDCHILDREN, line, action);
            else if(open && count > 1)
            {
                const int level = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL, line)) &
                                  QsciScintillaBase::SC_FOLDLEVELNUMBERMASK;
                const int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLASTCHILD, line, -1));
                for(int child = line + 1; child <= last; ++child)
                {
                    const int childLevel = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL, child)) &
                                           QsciScintillaBase::SC_FOLDLEVELNUMBERMASK;
                    if(isHeader(child) && childLevel - level < count)
                        m_editor->SendScintilla(QsciScintillaBase::SCI_FOLDLINE, child, action);
                }
            }
            m_editor->SendScintilla(QsciScintillaBase::SCI_FOLDLINE, line, action);
            if(open || recursive) break;
            line = parent(line);
        }
    }
    int visible = currentLine();
    while(visible >= 0 && !m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEVISIBLE, visible))
        visible = parent(visible);
    if(visible >= 0 && visible != currentLine())
    {
        setPosition(positionFromLine(visible));
        move("^", 1);
    }
}

void VimInputHandler::resetInsertCompletion()
{
    m_completionCandidates.clear();
    m_completionStart = m_completionEnd = m_completionIndex = 0;
}

void VimInputHandler::completeInsertWord(bool forward)
{
    if(m_replayDepth == 0 && !m_sequence.isEmpty())
    {
        // Even an unsuccessful completion must remain a no-op when repeated later.
        m_sequence.last().completionDeleteBytes = 0;
        m_sequence.last().completionText.clear();
    }
    // Completion never replaces an unrelated selection or modifies a read-only editor.
    if(m_editor->isReadOnly() || m_editor->hasSelectedText())
    {
        resetInsertCompletion();
        return;
    }

    if(m_completionCandidates.isEmpty())
    {
        const QString text = m_editor->text();
        const int caret = currentPosition();
        // Scintilla positions are UTF-8 bytes; Qt regular-expression offsets are UTF-16.
        const int characterCaret = QString::fromUtf8(text.toUtf8().left(caret)).size();
        const QRegularExpression words(QStringLiteral("\\w+"), QRegularExpression::UseUnicodePropertiesOption);
        auto matches = words.globalMatch(text);
        QVector<QPair<int, QString>> candidates;
        QString prefix;
        m_completionStart = caret;
        m_completionEnd = caret;
        while(matches.hasNext())
        {
            const auto match = matches.next();
            if(match.capturedStart() <= characterCaret && match.capturedEnd() >= characterCaret)
            {
                prefix = match.captured().left(characterCaret - match.capturedStart());
                m_completionStart = caret - prefix.toUtf8().size();
                continue; // The word being completed is not a source candidate.
            }
            candidates.append(qMakePair(match.capturedStart(), match.captured()));
        }

        // The untouched prefix is part of the cycle, so users can return to their input.
        m_completionCandidates.append(prefix);
        QSet<QString> seen;
        seen.insert(prefix);
        m_completionForward = forward;
        if(!forward) std::reverse(candidates.begin(), candidates.end());
        // Start in the requested direction and wrap, retaining the nearest occurrence
        // when the same candidate appears both before and after the caret.
        for(int pass = 0; pass < 2; ++pass)
            for(const auto& candidate : candidates)
            {
                if((candidate.first >= characterCaret) != ((pass == 0) == forward) ||
                   !candidate.second.startsWith(prefix) || seen.contains(candidate.second))
                    continue;
                seen.insert(candidate.second);
                m_completionCandidates.append(candidate.second);
            }
        m_completionIndex = 0;
        if(m_completionCandidates.size() == 1)
        {
            resetInsertCompletion();
            return;
        }
    }

    const QString previous = m_completionCandidates.at(m_completionIndex);
    m_completionIndex = (m_completionIndex + (forward == m_completionForward ? 1 : m_completionCandidates.size() - 1)) %
        m_completionCandidates.size();
    const QString replacement = m_completionCandidates.at(m_completionIndex);
    if(m_replayDepth == 0 && !m_sequence.isEmpty())
    {
        int common = 0;
        while(common < previous.size() && common < replacement.size() && previous.at(common) == replacement.at(common))
            ++common;
        // Do not split a supplementary character shared only up to its high surrogate.
        if(common > 0 && common < replacement.size() && replacement.at(common).isLowSurrogate())
            --common;
        m_sequence.last().completionDeleteBytes = previous.mid(common).toUtf8().size();
        m_sequence.last().completionText = replacement.mid(common);
    }
    // Signals from our replacement belong to this completion session, not an external edit.
    QScopedValueRollback<bool> changing(m_completionChanging, true);
    setSelection(m_completionStart, m_completionEnd);
    m_editor->replaceSelectedText(replacement);
    m_completionEnd = m_completionStart + replacement.toUtf8().size();
    setPosition(m_completionEnd);
}

bool VimInputHandler::handleCustomMapping(QKeyEvent* event)
{
    // A replacement character belongs to r, even if it is a mapping prefix.
    if(!m_pendingCommand.isEmpty())
        return false;
    if(event->modifiers().testFlag(Qt::ControlModifier) ||
       event->modifiers().testFlag(Qt::AltModifier) ||
       event->modifiers().testFlag(Qt::MetaModifier))
        return false;

    QString key = commandKey(event);
    if(key == m_leader) key = ",";
    if(key.isEmpty())
        return false;

    const QString candidate = m_mappingPrefix + key;
    if(executeCustomMapping(candidate))
    {
        m_mappingPrefix.clear();
        m_mappingTimer->stop();
        return true;
    }

    if(isCustomMappingPrefix(candidate))
    {
        m_mappingPrefix = candidate;
        if(m_mode == Mode::Insert || (m_mode != Mode::Normal && candidate == ";"))
            m_mappingTimer->start();
        else
            m_mappingTimer->stop();
        return true;
    }

    if(m_mappingPrefix.isEmpty())
        return false;

    if(m_mode == Mode::Insert || (m_mode != Mode::Normal && m_mappingPrefix == ";"))
        flushInsertMappingPrefix();
    else
        m_mappingPrefix.clear();

    // The key which failed to complete a mapping may itself start one.
    if(isCustomMappingPrefix(key))
    {
        m_mappingPrefix = key;
        if(m_mode == Mode::Insert || (m_mode != Mode::Normal && key == ";"))
            m_mappingTimer->start();
        else
            m_mappingTimer->stop();
        return true;
    }

    return false;
}

bool VimInputHandler::isCustomMappingPrefix(const QString& mapping) const
{
    QStringList mappings;
    if(m_mode == Mode::Insert)
        mappings = QStringList() << ",," << "z;" << "zh" << "zl" << "z,";
    else if(m_mode == Mode::Normal)
        mappings = QStringList() << ",," << ",ss" << ",ci" << ",xs" << ",xm" << ",xf"
                                 << "zh" << "zl" << "z;" << "z," << "zz" << "zt" << "zb"
                                 << "za" << "zo" << "zc" << "zO" << "zC" << "zR" << "zM" << "zv";
    else
        mappings = QStringList() << ",," << ",aa" << ",ci" << ",ss" << ";h" << ";q";

    const QString modePrefix = m_mode == Mode::Insert ? "i:" : m_mode == Mode::Normal ? "n:" : "x:";
    for(auto it = m_userMappings.constBegin(); it != m_userMappings.constEnd(); ++it)
        if(it.key().startsWith(modePrefix)) mappings.append(it.key().mid(2));
    for(const QString& candidate : mappings)
    {
        if(candidate.startsWith(mapping) && candidate != mapping)
            return true;
    }
    return false;
}

bool VimInputHandler::executeCustomMapping(const QString& mapping)
{
    const QString name = (m_mode == Mode::Insert ? "i:" : m_mode == Mode::Normal ? "n:" : "x:") + mapping;
    if(m_replayDepth == 0 && m_userMappings.contains(name))
    { m_mappingPrefix.clear(); playMapping(m_userMappings.value(name)); return true; }
    if(m_mode == Mode::Normal && mapping == ",xf")
    { if(auto* action = m_editor->window()->findChild<QAction*>("actionSqlOpenFile")) action->trigger(); return true; }
    if((m_mode == Mode::Visual || m_mode == Mode::VisualLine) && (mapping == ";h" || mapping == ";q"))
    {
        if(!m_editor->isReadOnly())
        {
            int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, std::min(m_visualAnchor, m_visualCaret)));
            int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, std::max(m_visualAnchor, m_visualCaret)));
            for(int line = last; line >= first; --line)
            {
                int start = positionFromLine(line), end = lineEndPosition(line);
                QString value = QString::fromUtf8(m_editor->text().toUtf8().mid(start, end-start));
                if(mapping == ";h")
                {
                    const auto match = QRegularExpression("^(\\s*)\\+ '([^']+)',*\\s*$").match(value);
                    if(!match.hasMatch()) continue;
                    value = match.captured(1)+match.captured(2);
                }
                else
                {
                    int indent = 0; while(indent < value.size() && value.at(indent).isSpace()) ++indent;
                    value = value.left(indent)+"+ '"+value.mid(indent).trimmed()+"'";
                }
                setSelection(start, end); m_editor->replaceSelectedText(value);
            }
            setPosition(positionFromLine(first));
        }
        setMode(Mode::Normal); return true;
    }
    if(m_mode == Mode::Normal && mapping == ",xm") { promptCommand(); return true; }
    if(m_mode == Mode::Normal && mapping.size() == 2 && mapping.at(0) == 'z' &&
       QStringLiteral("aocOCRMv").contains(mapping.at(1)))
    {
        foldCommand(mapping, takeCount());
        return true;
    }
    if(m_mode == Mode::Normal && (mapping == "zz" || mapping == "zt" || mapping == "zb"))
    {
        int visible = int(m_editor->SendScintilla(QsciScintillaBase::SCI_VISIBLEFROMDOCLINE, currentLine()));
        int height = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN));
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
            std::max(0, visible - (mapping == "zz" ? height/2 : mapping == "zb" ? height-1 : 0)));
        return true;
    }
    if(m_mode == Mode::Insert)
    {
        if(mapping == ",,")
        {
            if(currentPosition() > positionFromLine(currentLine()))
                setPosition(positionBefore(currentPosition()));
            setMode(Mode::Normal);
            clampNormalCaret();
            return true;
        }
        if(mapping == "zh")
        {
            move("^", 1);
            return true;
        }
        if(mapping == "zl")
        {
            setPosition(lineEndPosition(currentLine()));
            return true;
        }
        if(mapping == "z;" || mapping == "z,")
        {
            setPosition(lineEndPosition(currentLine()));
            m_editor->replaceSelectedText(mapping.right(1));
            return true;
        }
        return false;
    }

    if(mapping == ",,")
    {
        if(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
            setPosition(m_visualCaret);
        setMode(Mode::Normal);
        resetPendingCommand();
        return true;
    }

    if(m_mode == Mode::Normal)
    {
        if(mapping == "zh" || mapping == "zl")
            return move(mapping == "zh" ? "^" : "$", 1);

        if(mapping == "z;" || mapping == "z,")
        {
            if(m_editor->isReadOnly())
                return true;
            setPosition(lineEndPosition(currentLine()));
            m_editor->replaceSelectedText(mapping.right(1));
            setMode(Mode::Normal);
            clampNormalCaret();
            return true;
        }

        if(mapping == ",ss")
        {
            promptSearch(true);
            return true;
        }

        if(mapping == ",ci")
        {
            QMetaObject::invokeMethod(m_editor, "toggleBlockComment", Qt::DirectConnection);
            clampNormalCaret();
            return true;
        }

        if(mapping == ",xs")
        {
            if(QWidget* window = m_editor->window())
            {
                if(QAction* saveAction = window->findChild<QAction*>(QStringLiteral("actionSqlSaveFile")))
                    saveAction->trigger();
            }
            return true;
        }

        return false;
    }

    if(mapping == ",aa")
    {
        finishVisualOperator("y");
        return true;
    }
    if(mapping == ",ss")
    {
        setPosition(m_visualCaret);
        setMode(Mode::Normal);
        promptSearch(true);
        return true;
    }
    if(mapping == ",ci")
    {
        QMetaObject::invokeMethod(m_editor, "toggleBlockComment", Qt::DirectConnection);
        setPosition(m_visualCaret);
        setMode(Mode::Normal);
        clampNormalCaret();
        return true;
    }

    return false;
}

void VimInputHandler::flushInsertMappingPrefix()
{
    m_mappingTimer->stop();
    const QString prefix = m_mappingPrefix;
    m_mappingPrefix.clear();
    if(m_mode != Mode::Insert && m_mode != Mode::Normal && prefix == ";")
    {
        repeatFindMotion(false, takeCount());
        return;
    }
    if(m_mode == Mode::Insert && !m_editor->isReadOnly())
        for(const QChar character : prefix)
        {
            QKeyEvent event(QEvent::KeyPress, character.toUpper().unicode(), Qt::NoModifier, QString(character));
            forwardInsertKey(&event);
        }
}

void VimInputHandler::forwardInsertKey(QKeyEvent* event)
{
    const int start = currentPosition();
    const QByteArray before = m_replace ? m_editor->text().toUtf8() : QByteArray();
    const bool textKey = !event->text().isEmpty() &&
        !event->modifiers().testFlag(Qt::ControlModifier) &&
        !event->modifiers().testFlag(Qt::MetaModifier);
    if(textKey && !m_insertTextEntered)
        m_insertBackspaceStart = start;
    m_forwarding = true;
    QCoreApplication::sendEvent(m_editor, event);
    m_forwarding = false;
    if(textKey && currentPosition() > start)
        m_insertTextEntered = true;

    // Moving left of the insertion start makes that earlier position the new
    // backspacing boundary. Backspace itself deliberately keeps its column.
    const int key = event->key();
    if(key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
       key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End ||
       key == Qt::Key_PageUp || key == Qt::Key_PageDown)
        m_insertBackspaceStart = std::min(m_insertBackspaceStart, currentPosition());

    if(m_replace && textKey && currentPosition() > start)
    {
        const QByteArray after = m_editor->text().toUtf8();
        const int length = currentPosition() - start;
        const int removed = before.size() - after.size() + length;
        if(removed >= 0 && before.left(start) == after.left(start) &&
           before.mid(start + removed) == after.mid(start + length))
            m_replaceEdits.append({start, before.mid(start, removed), after.mid(start, length)});
    }
}

void VimInputHandler::backspaceInsert(bool wholeLine)
{
    if(m_editor->isReadOnly()) return;
    const int caret = currentPosition();
    const int lineStart = positionFromLine(currentLine());
    const int first = wholeLine && caret > lineStart
        ? (m_insertTextEntered && m_insertBackspaceStart > lineStart && m_insertBackspaceStart < caret
            ? m_insertBackspaceStart : lineStart)
        : positionBefore(caret);
    if(m_replace)
    {
        while(currentPosition() > first)
        {
            const int position = currentPosition();
            if(!m_replaceEdits.isEmpty())
            {
                const ReplaceEdit edit = m_replaceEdits.last();
                if(edit.start + edit.inserted.size() == position && edit.start <= position &&
                   m_editor->text().toUtf8().mid(edit.start, edit.inserted.size()) == edit.inserted)
                {
                    m_replaceEdits.removeLast();
                    setSelection(edit.start, position);
                    m_editor->replaceSelectedText(QString::fromUtf8(edit.original));
                    setPosition(edit.start);
                    continue;
                }
            }
            // Before the replaced run, Vim moves back over original text.
            setPosition(positionBefore(position));
        }
        return;
    }
    if(caret == lineStart || !wholeLine)
    {
        const int oldLength = documentLength();
        m_editor->SendScintilla(QsciScintillaBase::SCI_DELETEBACK);
        if(caret == lineStart && currentPosition() < caret)
            m_insertBackspaceStart = std::max(0, m_insertBackspaceStart - (oldLength - documentLength()));
    }
    else
    {
        setSelection(first, caret);
        m_editor->replaceSelectedText(QString());
    }
}

bool VimInputHandler::handleControlKey(QKeyEvent* event)
{
    const int explicitCount = m_count;
    const int count = takeCount();
    switch(event->key())
    {
    case Qt::Key_A:
    case Qt::Key_X:
    {
        if(m_editor->isReadOnly()) return true;
        const int line = currentLine(); const QString value = m_editor->text(line);
        const int byteOffset = currentPosition()-positionFromLine(line);
        const int offset = QString::fromUtf8(value.toUtf8().left(byteOffset)).size();
        auto matches = QRegularExpression("-?\\d+").globalMatch(value);
        while(matches.hasNext())
        {
            auto match = matches.next(); if(match.capturedEnd() <= offset) continue;
            bool ok; qlonglong number = match.captured().toLongLong(&ok);
            const int delta = event->key() == Qt::Key_A ? count : -count;
            if(!ok || (delta > 0 && number > std::numeric_limits<qlonglong>::max()-delta) ||
               (delta < 0 && number < std::numeric_limits<qlonglong>::min()-delta)) return true;
            int first = positionFromLine(line)+value.left(match.capturedStart()).toUtf8().size();
            setSelection(first, first+match.captured().toUtf8().size());
            m_editor->replaceSelectedText(QString::number(number+delta)); setPosition(first); return true;
        }
        return true;
    }
    case Qt::Key_V:
        if(m_mode == Mode::VisualBlock) { setMode(Mode::Normal); setPosition(m_visualCaret); }
        else { m_visualAnchor = currentPosition(); m_visualCaret = currentPosition();
            m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
            setMode(Mode::VisualBlock); updateVisualSelection(); }
        return true;
    case Qt::Key_O:
    case Qt::Key_I:
        if(!m_jumps.isEmpty())
        {
            m_jumpIndex = std::max(0, std::min(m_jumps.size()-1, m_jumpIndex + (event->key() == Qt::Key_O ? -1 : 1)));
            setPosition(m_jumps.at(m_jumpIndex)); clampNormalCaret();
        }
        return true;
    case Qt::Key_R:
        for(int i = 0; i < count; ++i) m_editor->redo();
        return true;
    case Qt::Key_D:
        move("j", explicitCount ? count : std::max(1, int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN)) / 2));
        clampNormalCaret();
        return true;
    case Qt::Key_U:
        move("k", explicitCount ? count : std::max(1, int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN)) / 2));
        clampNormalCaret();
        return true;
    case Qt::Key_F:
    case Qt::Key_B:
        for(int i = 0; i < count; ++i)
            m_editor->SendScintilla(event->key() == Qt::Key_F ? QsciScintillaBase::SCI_PAGEDOWN : QsciScintillaBase::SCI_PAGEUP);
        clampNormalCaret(); return true;
    case Qt::Key_W: return true;
    default:
        // Keep application shortcuts such as Ctrl+S, Ctrl+F and Ctrl+Enter.
        return false;
    }
}

bool VimInputHandler::handleNormalKey(QKeyEvent* event)
{
    const QString key = commandKey(event);

    if(!m_pendingCommand.isEmpty() && (m_pendingCommand == "r" || m_pendingCommand == "q" ||
       m_pendingCommand == "@" || m_pendingCommand == "\"" || m_pendingCommand == "m" ||
       m_pendingCommand == "'" || m_pendingCommand == "`" ||
       QString("fFtT").contains(m_pendingCommand.right(1))))
        return handlePendingKey(event);

    if(key.length() == 1 && key.at(0).isDigit() && !(key == "0" && m_count == 0))
    {
        m_count = std::min(MaximumCount, m_count * 10 + key.toInt());
        return true;
    }

    if(!m_pendingCommand.isEmpty())
        return handlePendingKey(event);

    if(extendedNormal(key)) return true;

    if(key == "d" || key == "y" || key == "c" || key == "g" || key == "r")
    {
        m_pendingCommand = key;
        m_pendingCount = takeCount();
        return true;
    }

    if(key == "i")
    {
        setMode(Mode::Insert);
        return true;
    }
    if(key == "I")
    {
        move("^", 1);
        setMode(Mode::Insert);
        return true;
    }
    if(key == "a")
    {
        const int lineEnd = lineEndPosition(currentLine());
        if(currentPosition() < lineEnd)
            setPosition(positionAfter(currentPosition()));
        setMode(Mode::Insert);
        return true;
    }
    if(key == "A")
    {
        setPosition(lineEndPosition(currentLine()));
        setMode(Mode::Insert);
        return true;
    }
    if(key == "o" || key == "O")
    {
        m_repeatNewline = true;
        if(m_editor->isReadOnly())
            return true;

        if(key == "o")
        {
            setPosition(lineEndPosition(currentLine()));
            m_editor->SendScintilla(QsciScintillaBase::SCI_NEWLINE);
        }
        else
        {
            setPosition(positionFromLine(currentLine()));
            m_editor->SendScintilla(QsciScintillaBase::SCI_NEWLINE);
            m_editor->SendScintilla(QsciScintillaBase::SCI_LINEUP);
        }
        setMode(Mode::Insert);
        return true;
    }
    if(key == "v" || key == "V")
    {
        enterVisualMode(key == "V");
        return true;
    }
    if(key == "x" || key == "s")
    {
        deleteCharacter(takeCount(), key == "s");
        return true;
    }
    if(key == "D" || key == "C")
    {
        m_pendingCommand = key == "D" ? "d" : "c";
        m_pendingCount = takeCount();
        applyOperatorMotion("$", m_pendingCount);
        resetPendingCommand();
        return true;
    }
    if(key == "Y")
    {
        m_pendingCommand = "y";
        m_pendingCount = takeCount();
        applyLineOperator(currentLine(), currentLine() + m_pendingCount - 1);
        resetPendingCommand();
        return true;
    }
    if(key == "p" || key == "P")
    {
        paste(key == "P", takeCount());
        return true;
    }
    if(key == "u")
    {
        const int count = takeCount();
        for(int i = 0; i < count; ++i)
            m_editor->undo();
        clampNormalCaret();
        return true;
    }
    if(key == "&")
    {
        const int count = takeCount();
        executeCommand("& " + QString::number(count));
        return true;
    }
    if(key == "J")
    {
        joinLines(takeCount());
        return true;
    }
    if(key == "~")
    {
        toggleCase(takeCount());
        return true;
    }
    if(key == "/" || key == "?")
    {
        promptSearch(key == "/");
        return true;
    }
    if(key == "n" || key == "N")
    {
        repeatSearch(key == "N");
        return true;
    }

    if(key == "G" && m_count > 0)
    {
        const int targetLine = std::min(m_editor->lines() - 1, m_count - 1);
        resetPendingCommand();
        m_editor->setCursorPosition(std::max(0, targetLine), 0);
        clampNormalCaret();
        return true;
    }

    const int count = takeCount();
    if(move(key, count))
        return true;

    // Normal mode consumes printable text so it can never accidentally edit.
    return !key.isEmpty();
}

bool VimInputHandler::handlePendingKey(QKeyEvent* event)
{
    const QString key = commandKey(event);
    if(extendedPending(key)) return true;
    if(handleSurroundKey(key)) return true;
    if(QString("fFtT").contains(key) && key.size() == 1 &&
       isMotionOperator(m_pendingCommand))
    { m_pendingCommand += key; return true; }
    if(key == ";" && isMotionOperator(m_pendingCommand))
    {
        repeatFindMotion(false, m_pendingCount * takeCount(), m_pendingCommand);
        m_selectedRegister.clear();
        resetPendingCommand();
        return true;
    }
    if(key == "s" && (m_pendingCommand == "y" || m_pendingCommand == "c" || m_pendingCommand == "d"))
    {
        m_pendingCommand += "s";
        return true;
    }


    if(m_pendingCommand.endsWith("i") || m_pendingCommand.endsWith("a"))
    {
        applyTextObject(key, m_pendingCommand.endsWith("a"), m_pendingCount * takeCount());
        resetPendingCommand();
        return true;
    }
    if((key == "i" || key == "a") &&
       (m_pendingCommand == "d" || m_pendingCommand == "c" || m_pendingCommand == "y" || m_pendingCommand == "gu" || m_pendingCommand == "gU" || m_pendingCommand == "g~"))
    {
        m_pendingCommand += key;
        return true;
    }

    if(m_pendingCommand == "r")
    {
        if(!key.isEmpty())
            replaceCharacter(key.left(1), m_pendingCount * takeCount());
        resetPendingCommand();
        return true;
    }

    if(m_pendingCommand.endsWith("g"))
    {
        // Recognized g motions are routed by extendedPending before this fallback.
        m_selectedRegister.clear();
        resetPendingCommand();
        return true;
    }

    if(key == "g")
    {
        m_pendingCommand += "g";
        return true;
    }

    const QString operation = m_pendingCommand;
    const int count = m_pendingCount * takeCount();

    if(key == operation)
        applyLineOperator(currentLine(), currentLine() + count - 1);
    else
        applyOperatorMotion(key, count);

    resetPendingCommand();
    return true;
}

bool VimInputHandler::handleVisualKey(QKeyEvent* event)
{
    const QString key = commandKey(event);
    if(m_pendingCommand.size() == 1 && QString("fFtT").contains(m_pendingCommand))
        return extendedPending(key); // The target can be a digit or a command letter.
    if(m_pendingCommand == "g")
    {
        if(handleGMotion(key)) return true;
        if(key.size() == 1 && key.at(0).isDigit() && !(key == "0" && m_count == 0))
        {
            m_count = std::min(MaximumCount, m_count * 10 + key.toInt());
            return true;
        }
        resetPendingCommand();
        if(key != "u" && key != "U" && key != "~") return true;
    }
    if(m_pendingCommand == "vi" || m_pendingCommand == "va")
    {
        setPosition(m_visualCaret);
        if(!applyTextObject(key, m_pendingCommand == "va", takeCount()))
            updateVisualSelection();
        resetPendingCommand();
        return true;
    }
    if(m_pendingCommand == "\"") return extendedPending(key);
    if(m_pendingCommand.isEmpty() && key == "\"")
    { m_pendingCommand = key; m_pendingCount = takeCount(); return true; }
    if(key == "/" || key == "?") { promptSearch(key == "/"); return true; }
    if(key == "n" || key == "N") { repeatSearch(key == "N"); return true; }
    if(key == ":") { promptCommand(); return true; }
    if(m_mode == Mode::VisualBlock && (key == "d" || key == "x" || key == "y" || key == "c" || key == "I" || key == "A"))
    { finishBlockOperator(key == "x" ? "d" : key); return true; }
    if(key == "p" || key == "P")
    {
        pasteVisual(key == "P", takeCount());
        return true;
    }
    if(key == "o") { std::swap(m_visualAnchor, m_visualCaret); updateVisualSelection(); return true; }
    if(key == "J")
    {
        int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, std::min(m_visualAnchor, m_visualCaret)));
        int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, std::max(m_visualAnchor, m_visualCaret)));
        setMode(Mode::Normal); setPosition(positionFromLine(first)); joinLines(last-first+1); return true;
    }
    if(key == ">" || key == "<" || key == "=" || key == "u" || key == "U" || key == "~")
    {
        int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
        int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));
        if(key == ">" || key == "<" || key == "=")
            indentLines(int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, first)),
                        int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, positionBefore(last))), key);
        else transformRange(first, last, key);
        setMode(Mode::Normal); return true;
    }
    if(m_pendingCommand == "vs")
    {
        finishSurround(key);
        return true;
    }
    if(key == "S")
    {
        m_surroundStart = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
        m_surroundEnd = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));
        m_pendingCommand = "vs";
        return true;
    }


    if(key == "i" || key == "a")
    {
        m_pendingCommand = "v" + key;
        return true;
    }


    if(key.length() == 1 && key.at(0).isDigit() && !(key == "0" && m_count == 0))
    {
        m_count = std::min(MaximumCount, m_count * 10 + key.toInt());
        return true;
    }

    if(key == "v")
    {
        if(m_mode == Mode::Visual)
        {
            setPosition(m_visualCaret);
            setMode(Mode::Normal);
        }
        else
        {
            m_mode = Mode::Visual;
            emit modeChanged();
            updateVisualSelection();
        }
        return true;
    }
    if(key == "V")
    {
        if(m_mode == Mode::VisualLine)
        {
            setPosition(m_visualCaret);
            setMode(Mode::Normal);
        }
        else
        {
            m_mode = Mode::VisualLine;
            emit modeChanged();
            updateVisualSelection();
        }
        return true;
    }
    if(key == "d" || key == "x" || key == "y" || key == "c")
    {
        finishVisualOperator(key == "x" ? "d" : key);
        return true;
    }

    if(key == "g" || (key.size() == 1 && QString("fFtT").contains(key)))
    {
        m_pendingCommand = key;
        m_pendingCount = takeCount();
        return true;
    }
    if(key == ";")
    {
        repeatFindMotion(false, takeCount());
        return true;
    }

    const int count = takeCount();
    setPosition(m_visualCaret);
    if(move(key, count))
    {
        m_visualCaret = currentPosition();
        updateVisualSelection();
        return true;
    }

    return !key.isEmpty();
}

void VimInputHandler::setMode(Mode mode)
{
    resetInsertCompletion();
    if(mode == Mode::Normal || mode == Mode::Insert) m_visualTagSelected = false;
    if(m_mode == Mode::Insert && mode == Mode::Normal)
    {
        finishBlockInsert(); m_replace = false; m_editor->setOverwriteMode(false);
        if(m_insertRepeat > 1 && !m_editor->isReadOnly())
        {
            const QByteArray before = m_insertBefore.toUtf8(), after = m_editor->text().toUtf8();
            const int length = after.size()-before.size();
            if(length > 0 && after.left(m_insertStart) == before.left(m_insertStart) &&
               after.mid(m_insertStart+length) == before.mid(m_insertStart))
            {
                QString value = QString::fromUtf8(after.mid(m_insertStart, length));
                if(m_repeatNewline) value.prepend(endOfLine());
                setPosition(m_insertStart+length); m_editor->replaceSelectedText(value.repeated(m_insertRepeat-1));
                setPosition(positionBefore(currentPosition()));
            }
        }
        m_insertRepeat = 1; m_repeatNewline = false;
    }
    if(m_mode != Mode::Insert && mode == Mode::Insert)
    {
        m_insertRepeat = takeCount(); m_insertStart = currentPosition(); m_insertBefore = m_editor->text();
        m_insertBackspaceStart = m_insertStart; m_insertTextEntered = false; m_replaceEdits.clear();
    }
    if(m_mode == mode)
    {
        emit modeChanged();
        return;
    }

    if(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
    {
        m_savedAnchor = m_visualAnchor; m_savedCaret = m_visualCaret; m_savedVisualMode = m_mode;
        m_marks["<"] = std::min(m_visualAnchor, m_visualCaret);
        m_marks[">"] = std::max(m_visualAnchor, m_visualCaret);
    }
    if(m_mode == Mode::VisualBlock && mode != Mode::VisualBlock)
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSELECTIONMODE, QsciScintillaBase::SC_SEL_STREAM);
    m_mode = mode;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETCARETSTYLE,
                            mode == Mode::Insert ? QsciScintillaBase::CARETSTYLE_LINE
                                                 : QsciScintillaBase::CARETSTYLE_BLOCK);
    emit modeChanged();
}

void VimInputHandler::resetPendingCommand()
{
    m_pendingCommand.clear();
    m_pendingCount = 1;
    m_count = 0;
}

int VimInputHandler::takeCount()
{
    const int count = m_count == 0 ? 1 : m_count;
    m_count = 0;
    return count;
}

int VimInputHandler::currentPosition() const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
}

int VimInputHandler::documentLength() const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLENGTH));
}

int VimInputHandler::currentLine() const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, currentPosition()));
}

int VimInputHandler::positionFromLine(int line) const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line));
}

int VimInputHandler::lineEndPosition(int line) const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION, line));
}

int VimInputHandler::positionAfter(int position) const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONAFTER, position));
}

int VimInputHandler::positionBefore(int position) const
{
    return static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONBEFORE, position));
}

int VimInputHandler::characterClassAt(int position) const
{
    if(position < 0 || position >= documentLength())
        return 0;

    const unsigned char character = static_cast<unsigned char>(
        m_editor->SendScintilla(QsciScintillaBase::SCI_GETCHARAT, position));
    if(character == 0 || std::isspace(character))
        return 0;
    if(character >= 0x80 || std::isalnum(character) || character == '_')
        return 1;
    return 2;
}

int VimInputHandler::nextWordEndPosition(int position) const
{
    if(documentLength() == 0)
        return 0;

    int cursor = std::max(0, std::min(documentLength() - 1, position));

    // Vim's e always advances before looking for an end. This is important
    // when the caret is already on the final character of a word.
    if(cursor < documentLength() - 1)
        cursor = positionAfter(cursor);

    while(cursor < documentLength() && characterClassAt(cursor) == 0)
    {
        const int next = positionAfter(cursor);
        if(next <= cursor)
            break;
        cursor = next;
    }

    if(cursor >= documentLength())
        return std::max(0, positionBefore(documentLength()));

    const int characterClass = characterClassAt(cursor);
    while(cursor < documentLength() - 1)
    {
        const int next = positionAfter(cursor);
        if(next <= cursor || characterClassAt(next) != characterClass)
            break;
        cursor = next;
    }
    return cursor;
}

void VimInputHandler::setPosition(int position)
{
    const int safePosition = std::max(0, std::min(documentLength(), position));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION, safePosition);
}

void VimInputHandler::setSelection(int start, int end)
{
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                            std::max(0, std::min(documentLength(), start)),
                            std::max(0, std::min(documentLength(), end)));
}

void VimInputHandler::clampNormalCaret()
{
    if(m_mode == Mode::Insert)
        return;

    const int line = currentLine();
    const int lineStart = positionFromLine(line);
    const int lineEnd = lineEndPosition(line);
    if(lineEnd > lineStart && currentPosition() >= lineEnd)
        setPosition(positionBefore(lineEnd));
}

bool VimInputHandler::move(const QString& command, int count)
{
    if(command == "gg" || command == "g_")
    {
        int lastLine = m_editor->lines() - 1;
        if(lastLine > 0 && positionFromLine(lastLine) == documentLength()) --lastLine;
        const int line = command == "gg" ? std::min(lastLine, std::max(1, count) - 1) :
            std::min(lastLine, currentLine() + std::min(std::max(1, count) - 1, lastLine));
        setPosition(positionFromLine(line));
        if(command == "gg") move("^", 1);
        else
        {
            int end = lineEndPosition(line);
            while(end > positionFromLine(line) && characterClassAt(positionBefore(end)) == 0)
                end = positionBefore(end);
            setPosition(end > positionFromLine(line) ? positionBefore(end) : end);
        }
        m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
        return true;
    }
    if(command == "+" || command == "-" || command == "\r" || command == "_")
    {
        const int offset = command == "-" ? -count : command == "_" ? count - 1 : count;
        const int line = std::max(0, std::min(m_editor->lines() - 1, currentLine() + offset));
        setPosition(positionFromLine(line));
        move("^", 1);
        return true;
    }
    if(command == "ge" || command == "gE")
    {
        int position = currentPosition();
        auto category = [&](int p) {
            const int line = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, p));
            // Vim treats an empty line as a word, including its CRLF boundary.
            if(p == positionFromLine(line) && p == lineEndPosition(line)) return 3;
            const int c = characterClassAt(p);
            return command == "gE" && c ? 1 : c;
        };
        for(int i = 0; i < count && position > 0; ++i)
        {
            const int c = category(position);
            while(position > 0 && category(positionBefore(position)) == c)
                position = positionBefore(position);
            if(position > 0) position = positionBefore(position);
            while(position > 0 && category(position) == 0) position = positionBefore(position);
        }
        setPosition(position);
        m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
        return true;
    }
    if(command == "(" || command == ")")
    {
        const QByteArray bytes = m_editor->text().toUtf8();
        int position = currentPosition();
        auto boundary = [&](int p) {
            if(p == 0) return true;
            int prior = positionBefore(p);
            while(prior > 0 && characterClassAt(prior) == 0) prior = positionBefore(prior);
            return prior < positionBefore(p) && QByteArray(".!?").contains(bytes.at(prior));
        };
        for(int n = 0; n < count; ++n)
        {
            do { position = command == ")" ? positionAfter(position) : positionBefore(position); }
            while(position > 0 && position < documentLength() && (characterClassAt(position) == 0 || !boundary(position)));
        }
        setPosition(position); clampNormalCaret(); return true;
    }
    if(command == "{" || command == "}")
    {
        int line = currentLine(), direction = command == "}" ? 1 : -1;
        for(int n = 0; n < count; ++n)
        {
            line += direction;
            while(line > 0 && line < m_editor->lines()-1 && m_editor->text(line).trimmed().isEmpty()) line += direction;
            while(line > 0 && line < m_editor->lines()-1 && !m_editor->text(line).trimmed().isEmpty()) line += direction;
            line = std::max(0, std::min(line, m_editor->lines()-1));
        }
        setPosition(positionFromLine(line)); return true;
    }
    if(command == "H" || command == "M" || command == "L")
    {
        int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        int height = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN));
        int visible = first + (command == "H" ? count-1 : command == "M" ? height/2 : height-count);
        int line = int(m_editor->SendScintilla(QsciScintillaBase::SCI_DOCLINEFROMVISIBLE, std::max(0, visible)));
        setPosition(positionFromLine(std::min(m_editor->lines()-1, std::max(0, line)))); move("^", 1); return true;
    }

    count = std::max(1, count);

    if(command == "h")
    {
        for(int i = 0; i < count && currentPosition() > positionFromLine(currentLine()); ++i)
            setPosition(positionBefore(currentPosition()));
    }
    else if(command == "l")
    {
        for(int i = 0; i < count; ++i)
        {
            const int end = lineEndPosition(currentLine());
            if(currentPosition() >= end || positionAfter(currentPosition()) >= end)
                break;
            setPosition(positionAfter(currentPosition()));
        }
    }
    else if(command == "j" || command == "k")
    {
        const int message = command == "j" ? QsciScintillaBase::SCI_LINEDOWN : QsciScintillaBase::SCI_LINEUP;
        for(int i = 0; i < count; ++i)
            m_editor->SendScintilla(message);
        clampNormalCaret();
    }
    else if(command == "0")
    {
        m_editor->SendScintilla(QsciScintillaBase::SCI_HOME);
    }
    else if(command == "^")
    {
        setPosition(static_cast<int>(m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETLINEINDENTPOSITION, currentLine())));
        clampNormalCaret();
    }
    else if(command == "$")
    {
        for(int i = 1; i < count; ++i)
            m_editor->SendScintilla(QsciScintillaBase::SCI_LINEDOWN);
        m_editor->SendScintilla(QsciScintillaBase::SCI_LINEEND);
        clampNormalCaret();
    }
    else if(command == "w" || command == "b" || command == "e" ||
            command == "W" || command == "B" || command == "E")
    {
        const bool bigWord = command == command.toUpper();
        const QString motion = command.toLower();
        auto category = [this, bigWord](int p) {
            const int c = characterClassAt(p);
            return bigWord && c != 0 ? 1 : c;
        };
        for(int i = 0; i < count; ++i)
        {
            int p = currentPosition();
            if(motion == "b")
            {
                if(p > 0) p = positionBefore(p);
                while(p > 0 && category(p) == 0) p = positionBefore(p);
                const int c = category(p);
                while(p > 0 && category(positionBefore(p)) == c)
                    p = positionBefore(p);
            }
            else if(motion == "w")
            {
                const int c = category(p);
                while(p < documentLength() && category(p) == c)
                    p = positionAfter(p);
                while(p < documentLength() && category(p) == 0)
                    p = positionAfter(p);
            }
            else
            {
                if(p < documentLength()) p = positionAfter(p);
                while(p < documentLength() && category(p) == 0)
                    p = positionAfter(p);
                const int c = category(p);
                while(p < documentLength() && positionAfter(p) < documentLength() &&
                      category(positionAfter(p)) == c)
                    p = positionAfter(p);
                if(p >= documentLength()) p = positionBefore(documentLength());
            }
            setPosition(p);
        }
        if(m_pendingCommand.isEmpty()) clampNormalCaret();
    }
    else if(command == "G")
    {
        m_editor->setCursorPosition(std::max(0, m_editor->lines() - 1), 0);
        clampNormalCaret();
    }
    else if(command == "%")
    {
        int position = currentPosition();
        int match = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_BRACEMATCH, position));
        if(match < 0 && position < documentLength())
            match = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_BRACEMATCH, positionAfter(position)));
        if(match >= 0)
            setPosition(match);
    }
    else
    {
        return false;
    }

    // Horizontal motions set the column used by a subsequent j/k. Vertical
    // motions retain it so long-short-long line traversal returns to that column.
    if(command != "j" && command != "k")
        m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
    return true;
}

bool VimInputHandler::handleSurroundKey(const QString& key)
{
    if(m_pendingCommand == "ys-ready" || m_pendingCommand == "cs-ready")
    {
        finishSurround(key);
        return true;
    }
    if(m_pendingCommand == "ds" || m_pendingCommand == "cs")
    {
        const bool change = m_pendingCommand == "cs";
        if(!QStringLiteral("()[]{}<>bB\"'`").contains(key) || key.size() != 1 ||
           !textObjectRange(key, true, m_pendingCount * takeCount(), m_surroundStart, m_surroundEnd))
            resetPendingCommand();
        else if(change)
            m_pendingCommand = "cs-ready";
        else
            finishSurround(QString(), true);
        return true;
    }
    if(m_pendingCommand == "ysi" || m_pendingCommand == "ysa")
    {
        if(textObjectRange(key, m_pendingCommand == "ysa", m_pendingCount * takeCount(),
                           m_surroundStart, m_surroundEnd))
            m_pendingCommand = "ys-ready";
        else
            resetPendingCommand();
        return true;
    }
    if(m_pendingCommand != "ys") return false;
    if(key == "i" || key == "a")
    {
        m_pendingCommand += key;
        return true;
    }
    const int count = m_pendingCount * takeCount();
    if(key == "s")
    {
        m_surroundStart = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEINDENTPOSITION, currentLine()));
        m_surroundEnd = lineEndPosition(std::min(m_editor->lines() - 1, currentLine() + count - 1));
    }
    else
    {
        // Characterwise motions only. Linewise motions require separate layout semantics.
        if(key == "j" || key == "k" || key == "G" || !applyOperatorMotion(key, count))
        {
            resetPendingCommand();
            return true;
        }
        setPosition(m_surroundStart);
    }
    m_pendingCommand = "ys-ready";
    return true;
}

void VimInputHandler::finishSurround(const QString& delimiter, bool remove)
{
    QString left, right;
    if(!remove)
    {
        QString key = delimiter;
        if(key == "b") key = ")";
        if(key == "B") key = "}";
        const QString opens = "([{<";
        const QString closes = ")]}>";
        int pair = opens.indexOf(key);
        const bool padded = pair >= 0 && pair < 3;
        if(pair < 0) pair = closes.indexOf(key);
        if(key.size() == 1 && pair >= 0)
        {
            left = opens.mid(pair, 1) + (padded ? " " : "");
            right = (padded ? " " : "") + closes.mid(pair, 1);
        }
        else if(key == "\"" || key == "'" || key == "`")
            left = right = key;
        else
        {
            resetPendingCommand();
            return;
        }
    }
    const bool replacing = remove || m_pendingCommand == "cs-ready";
    const int first = m_surroundStart;
    const int last = m_surroundEnd;
    if(m_editor->isReadOnly() || first < 0 || last > documentLength() || last < first ||
       (replacing && last - first < 2))
    {
        resetPendingCommand();
        return;
    }
    int innerStart = first + (replacing ? 1 : 0);
    int innerEnd = last - (replacing ? 1 : 0);
    // Removing/changing a padded bracket also removes its adjacent padding.
    const QByteArray bytes = m_editor->text().toUtf8();
    if(replacing && QStringLiteral("([{ ").contains(QChar(bytes.at(first))))
    {
        while(innerStart < innerEnd && (bytes.at(innerStart) == ' ' || bytes.at(innerStart) == '\t')) ++innerStart;
        while(innerEnd > innerStart && (bytes.at(innerEnd - 1) == ' ' || bytes.at(innerEnd - 1) == '\t')) --innerEnd;
    }
    const QString body = QString::fromUtf8(bytes.mid(innerStart, innerEnd - innerStart));
    m_editor->beginUndoAction();
    setSelection(first, last);
    m_editor->replaceSelectedText(left + body + right);
    m_editor->endUndoAction();
    setMode(Mode::Normal);
    setPosition(first);
    clampNormalCaret();
    resetPendingCommand();
}

bool VimInputHandler::textObjectRange(const QString& object, bool around, int count, int& first, int& last) const
{
    const int caret = currentPosition();
    first = caret;
    last = caret;
    if(object == "t")
    {
        const bool expanding = m_pendingCommand.startsWith("v") &&
                               (m_visualAnchor != m_visualCaret || m_visualTagSelected);
        return tagObjectRange(m_editor->text().toUtf8(), caret, around, std::max(1, count), expanding,
                              std::min(m_visualAnchor, m_visualCaret),
                              positionAfter(std::max(m_visualAnchor, m_visualCaret)), first, last);
    }
    if(object == "p" || object == "s")
    {
        // Byte ranges match Scintilla's positions. ASCII delimiters cannot split
        // a UTF-8 character, and line boundaries preserve complete CRLF pairs.
        struct Part { int first; int last; bool space; };
        QVector<Part> parts;
        const QByteArray bytes = m_editor->text().toUtf8();
        if(bytes.isEmpty()) return false;
        const auto horizontalSpace = [](char c) { return c == ' ' || c == '\t'; };
        const auto space = [&horizontalSpace](char c) {
            return horizontalSpace(c) || c == '\r' || c == '\n';
        };
        const auto paragraphMacro = [this](int line) {
            const QString text = m_editor->text(line).section(QRegularExpression("[\\r\\n]"), 0, 0);
            if(text.startsWith(QChar('\f'))) return true;
            if(!text.startsWith('.')) return false;
            // Vim's default nroff paragraph and section macros.
            const QString macros = "IPLPPPQPP TPHPLIPpLpItpplpipbpSHNHH HUnhsh";
            const QString name = text.mid(1, 2).leftJustified(2, ' ');
            for(int i = 0; i + 1 < macros.size(); i += 2)
                if(name == macros.mid(i, 2)) return true;
            return false;
        };
        int lineCount = m_editor->lines();
        // Scintilla exposes a virtual empty line after a final EOL; it isn't an
        // extra paragraph and must never consume a count.
        if(lineCount > 1 && positionFromLine(lineCount - 1) == bytes.size()) --lineCount;
        if(object == "p")
        {
            for(int line = 0; line < lineCount; )
            {
                const int begin = line;
                const bool blank = m_editor->text(line).trimmed().isEmpty();
                ++line;
                while(line < lineCount && m_editor->text(line).trimmed().isEmpty() == blank &&
                      (blank || !paragraphMacro(line))) ++line;
                parts.append({positionFromLine(begin),
                              line < m_editor->lines() ? positionFromLine(line) : bytes.size(), blank});
            }
        }
        else
        {
            const int sentenceEnd = lineEndPosition(lineCount - 1) == positionFromLine(lineCount - 1)
                                      ? bytes.size() : lineEndPosition(lineCount - 1);
            int begin = 0;
            while(begin < sentenceEnd)
            {
                const int line = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, begin));
                const int lineStart = positionFromLine(line);
                const int lineEnd = lineEndPosition(line);
                if(begin == lineStart && lineStart == lineEnd)
                {
                    const int end = line + 1 < m_editor->lines() ? positionFromLine(line + 1) : sentenceEnd;
                    parts.append({begin, end, false});
                    parts.append({end, end, true});
                    begin = end;
                    continue;
                }
                int end = begin;
                while(end < sentenceEnd)
                {
                    const char c = bytes.at(end);
                    if(c == '.' || c == '!' || c == '?')
                    {
                        int after = end + 1;
                        while(after < sentenceEnd && QByteArray(")]\"'").contains(bytes.at(after))) ++after;
                        if(after == sentenceEnd || space(bytes.at(after)))
                        {
                            end = after;
                            break;
                        }
                    }
                    if(c == '\r' || c == '\n')
                    {
                        const int next = end + (c == '\r' && end + 1 < sentenceEnd && bytes.at(end + 1) == '\n' ? 2 : 1);
                        const int nextLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, next));
                        if(next == sentenceEnd || lineEndPosition(nextLine) == next || paragraphMacro(nextLine))
                        {
                            end = next;
                            break;
                        }
                    }
                    ++end;
                }
                // At a sentence-ending EOL Vim includes that complete EOL in
                // the characterwise object, but preserves the final file EOL.
                const int endLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, end));
                if(end < sentenceEnd && end != positionFromLine(endLine) &&
                   (bytes.at(end) == '\r' || bytes.at(end) == '\n'))
                {
                    const int next = end + (bytes.at(end) == '\r' && end + 1 < sentenceEnd && bytes.at(end + 1) == '\n' ? 2 : 1);
                    if(next < sentenceEnd) end = next;
                }
                parts.append({begin, end, false});
                int after = end;
                while(after < sentenceEnd && space(bytes.at(after)))
                {
                    const int blankLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, after));
                    if(positionFromLine(blankLine) == after && lineEndPosition(blankLine) == after) break;
                    ++after;
                }
                parts.append({end, after, true});
                if(after <= begin) break;
                begin = after;
            }
        }
        int index = 0;
        while(index + 1 < parts.size() && parts.at(index).last <= caret) ++index;
        while(index + 1 < parts.size() && parts.at(index).first == parts.at(index).last) ++index;
        int endIndex = index;
        first = parts.at(index).first;
        if(!around)
            endIndex = std::min(parts.size() - 1, index + std::max(1, count) - 1);
        else
        {
            const bool leadingSpace = parts.at(index).space;
            if(leadingSpace && index + 1 == parts.size()) return false;
            int remaining = std::max(1, count);
            while(endIndex < parts.size())
            {
                if(!parts.at(endIndex).space && --remaining == 0) break;
                ++endIndex;
            }
            endIndex = std::min(endIndex, parts.size() - 1);
            if(!leadingSpace)
            {
                if(endIndex + 1 < parts.size() && parts.at(endIndex + 1).space &&
                   parts.at(endIndex + 1).last > parts.at(endIndex + 1).first)
                    ++endIndex;
                else if(index > 0 && parts.at(index - 1).space)
                    first = parts.at(index - 1).first;
            }
        }
        last = parts.at(endIndex).last;
        return last > first;
    }
    if(object == "w" || object == "W")
    {
        if(caret >= documentLength()) return false;
        const auto category = [this, &object](int p) {
            const int c = characterClassAt(p);
            return object == "W" && c != 0 ? 1 : c;
        };
        const int initial = category(caret);
        while(first > 0 && category(positionBefore(first)) == initial)
            first = positionBefore(first);
        while(last < documentLength() && category(last) == initial)
            last = positionAfter(last);
        for(int i = 1; i < count; ++i)
        {
            while(last < documentLength() && category(last) == 0)
                last = positionAfter(last);
            const int c = category(last);
            while(last < documentLength() && category(last) == c)
                last = positionAfter(last);
        }
        if(around)
        {
            const int beforeSpace = last;
            while(last < documentLength() && category(last) == 0)
                last = positionAfter(last);
            if(last == beforeSpace)
                while(first > 0 && category(positionBefore(first)) == 0)
                    first = positionBefore(first);
        }
    }
    else
    {
        const QByteArray bytes = m_editor->text().toUtf8();
        const QString opens = "([{<";
        const QString closes = ")]}>";
        int kind = opens.indexOf(object);
        if(kind < 0) kind = closes.indexOf(object);
        if(object == "b") kind = 0;
        if(object == "B") kind = 2;
        if(kind >= 0 && object.size() == 1)
        {
            const char open = opens.at(kind).toLatin1();
            const char close = closes.at(kind).toLatin1();
            int depth = 0;
            int remaining = count;
            first = -1;
            // A closing delimiter under the caret belongs to its own pair.
            int scan = std::min(caret, bytes.size() - 1);
            if(scan >= 0 && scan < bytes.size() && bytes.at(scan) == close) --scan;
            for(int p = scan; p >= 0; --p)
            {
                if(bytes.at(p) == close) ++depth;
                if(bytes.at(p) == open)
                {
                    if(depth > 0) --depth;
                    else if(--remaining == 0) { first = p; break; }
                }
            }
            if(first < 0) return false;
            depth = 1;
            last = first + 1;
            for(; last < bytes.size(); ++last)
            {
                if(bytes.at(last) == open) ++depth;
                if(bytes.at(last) == close && --depth == 0) break;
            }
            if(last >= bytes.size() || last < caret) return false;
        }
        else if(object == "\"" || object == "'" || object == "`")
        {
            if(count != 1) return false;
            const char quote = object.at(0).toLatin1();
            first = -1;
            last = -1;
            int opening = -1;
            for(int p = positionFromLine(currentLine()); p < lineEndPosition(currentLine()); ++p)
            {
                if(bytes.at(p) != quote) continue;
                int slashes = 0;
                for(int q = p - 1; q >= 0 && bytes.at(q) == '\\'; --q) ++slashes;
                if(slashes % 2) continue;
                if(opening < 0) opening = p;
                else
                {
                    if(p >= caret) { first = opening; last = p; break; }
                    opening = -1;
                }
            }
            if(first < 0) return false;
        }
        else return false;
        if(around) ++last;
        else ++first;
    }
    return true;
}

bool VimInputHandler::applyTextObject(const QString& object, bool around, int count)
{
    int first, last;
    if(!textObjectRange(object, around, count, first, last)) return false;
    if(m_pendingCommand.startsWith("v"))
    {
        if(last <= first) return false;
        if((object == "p" || object == "s") && m_visualAnchor != m_visualCaret)
        {
            const bool forward = m_visualCaret >= m_visualAnchor;
            const int oldFirst = std::min(m_visualAnchor, m_visualCaret);
            const int oldLast = positionAfter(std::max(m_visualAnchor, m_visualCaret));
            if(first >= oldFirst && last <= oldLast)
            {
                const int next = forward ? oldLast : positionBefore(oldFirst);
                if((forward && next < documentLength()) || (!forward && oldFirst > 0))
                {
                    const int saved = currentPosition();
                    setPosition(next);
                    textObjectRange(object, around, count, first, last);
                    setPosition(saved);
                }
            }
            first = std::min(first, oldFirst);
            last = std::max(last, oldLast);
            m_visualAnchor = forward ? first : positionBefore(last);
            m_visualCaret = forward ? positionBefore(last) : first;
        }
        else
        {
            m_visualAnchor = first;
            m_visualCaret = positionBefore(last);
        }
        m_visualTagSelected = object == "t";
        m_mode = object == "p" ? Mode::VisualLine : Mode::Visual;
        updateVisualSelection();
        emit modeChanged();
    }
    else if(object == "p")
    {
        const QString pending = m_pendingCommand;
        // Line operators expect their base command, without the text object.
        m_pendingCommand.chop(1);
        const int firstLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, first));
        const int lastLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, positionBefore(last)));
        const bool removeFinalEol = m_pendingCommand == "d" && !m_editor->isReadOnly() &&
                                    firstLine > 0 && lastLine == m_editor->lines() - 1 &&
                                    lineEndPosition(lastLine) == documentLength();
        if(removeFinalEol) m_editor->beginUndoAction();
        applyLineOperator(firstLine, lastLine);
        if(removeFinalEol)
        {
            // SCI_LINEDELETE leaves the preceding separator when the last
            // physical line has no EOL; Vim's linewise paragraph delete doesn't.
            const int end = documentLength();
            int begin = end;
            const QByteArray text = m_editor->text().toUtf8();
            if(begin > 0 && text.at(begin - 1) == '\n') --begin;
            if(begin > 0 && text.at(begin - 1) == '\r') --begin;
            if(begin < end) { setSelection(begin, end); m_editor->removeSelectedText(); }
            m_editor->endUndoAction();
            clampNormalCaret();
        }
        m_pendingCommand = pending;
    }
    else if(last == first && m_pendingCommand.startsWith("c"))
    {
        setPosition(first);
        setMode(Mode::Insert);
    }
    else
        applyCharacterOperator(first, last);
    return true;
}

bool VimInputHandler::applyOperatorMotion(const QString& command, int count)
{
    const int start = currentPosition();
    const int startLine = currentLine();

    if(command == "gg")
    {
        move(command, count);
        const int targetLine = currentLine();
        setPosition(start);
        applyLineOperator(std::min(startLine, targetLine), std::max(startLine, targetLine));
        return true;
    }

    if(command == "+" || command == "-" || command == "\r" || command == "_")
    {
        move(command, count);
        const int targetLine = currentLine();
        setPosition(start);
        if(targetLine == startLine && !(command == "_" && count == 1)) return false;
        applyLineOperator(std::min(startLine, targetLine), std::max(startLine, targetLine));
        return true;
    }
    if(command == "j" || command == "k" || command == "G")
    {
        if(command == "G")
            applyLineOperator(startLine, m_editor->lines() - 1);
        else if(command == "j")
            applyLineOperator(startLine, startLine + count);
        else
            applyLineOperator(startLine - count, startLine);
        return true;
    }

    // Vim treats cw/cW on non-whitespace as ce/cE: preserve the separator.
    if(m_pendingCommand.startsWith("c") && (command == "w" || command == "W") &&
       characterClassAt(start) != 0)
    {
        int target = start;
        const auto category = [this, &command](int p) {
            const int c = characterClassAt(p);
            return command == "W" && c != 0 ? 1 : c;
        };
        for(int i = 0; i < count; ++i)
        {
            const int c = category(target);
            while(target < documentLength() && category(target) == c)
                target = positionAfter(target);
            if(i + 1 < count)
                while(target < documentLength() && category(target) == 0)
                    target = positionAfter(target);
        }
        applyCharacterOperator(start, target);
        return true;
    }

    if(!move(command, count))
    {
        setPosition(start);
        return false;
    }

    int target = command == "$" ? lineEndPosition(currentLine()) : currentPosition();
    if((command == "ge" || command == "gE") && target == start) return false;
    if((command == "e" || command == "E") && target >= start)
        target = positionAfter(target);
    int first = std::min(start, target);
    int last = std::max(start, target);
    if((command == "%" || command == "ge" || command == "gE" || command == "g_") &&
       last < documentLength() &&
       (command != "g_" || last < lineEndPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, last)))))
        last = positionAfter(last);
    applyCharacterOperator(first, last);
    return true;
}

void VimInputHandler::applyCharacterOperator(int start, int end)
{
    if(m_pendingCommand == "ys")
    {
        m_surroundStart = start;
        m_surroundEnd = end;
        return;
    }

    if(end <= start)
    {
        setPosition(start);
        return;
    }

    if(m_pendingCommand.startsWith("gu") || m_pendingCommand.startsWith("gU") || m_pendingCommand.startsWith("g~"))
    { transformRange(start, end, m_pendingCommand.mid(1, 1)); return; }
    if(m_pendingCommand == ">" || m_pendingCommand == "<" || m_pendingCommand == "=")
    { indentLines(int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, start)),
                  int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, positionBefore(end))), m_pendingCommand); return; }
    setSelection(start, end);
    setRegister(m_editor->selectedText(), false, m_pendingCommand.startsWith("y"));

    if(m_pendingCommand.startsWith("y"))
    {
        setPosition(start);
        clampNormalCaret();
        return;
    }

    if(!m_editor->isReadOnly())
        m_editor->replaceSelectedText(QString());
    setPosition(start);

    if(m_pendingCommand.startsWith("c"))
        setMode(Mode::Insert);
    else
        clampNormalCaret();
}

void VimInputHandler::applyLineOperator(int firstLine, int lastLine)
{
    firstLine = std::max(0, std::min(m_editor->lines() - 1, firstLine));
    lastLine = std::max(0, std::min(m_editor->lines() - 1, lastLine));
    if(firstLine > lastLine)
        std::swap(firstLine, lastLine);

    if(m_pendingCommand == "gu" || m_pendingCommand == "gU" || m_pendingCommand == "g~")
    { transformRange(positionFromLine(firstLine), lineEndPosition(lastLine), m_pendingCommand.right(1)); return; }
    if(m_pendingCommand == ">" || m_pendingCommand == "<" || m_pendingCommand == "=")
    { indentLines(firstLine, lastLine, m_pendingCommand); return; }
    setRegister(linesText(firstLine, lastLine), true, m_pendingCommand.startsWith("y"));
    // Collapse any visual selection before sending SCI_LINEDELETE repeatedly.
    setPosition(positionFromLine(firstLine));

    if(m_pendingCommand.startsWith("y"))
        return;

    if(m_editor->isReadOnly())
        return;

    m_editor->beginUndoAction();
    for(int line = firstLine; line <= lastLine; ++line)
        m_editor->SendScintilla(QsciScintillaBase::SCI_LINEDELETE);

    if(m_pendingCommand.startsWith("c"))
    {
        if(!m_editor->text().isEmpty())
        {
            m_editor->insertAt(endOfLine(), std::min(firstLine, m_editor->lines() - 1), 0);
            m_editor->setCursorPosition(std::min(firstLine, m_editor->lines() - 1), 0);
        }
        setMode(Mode::Insert);
    }
    else
    {
        clampNormalCaret();
    }
    m_editor->endUndoAction();
}

void VimInputHandler::deleteCharacter(int count, bool enterInsertMode)
{
    if(m_editor->isReadOnly())
        return;

    const int start = currentPosition();
    const int lineEnd = lineEndPosition(currentLine());
    int end = start;
    for(int i = 0; i < count && end < lineEnd; ++i)
        end = positionAfter(end);

    if(end > start)
    {
        setSelection(start, end);
        setRegister(m_editor->selectedText(), false);
        m_editor->replaceSelectedText(QString());
        setPosition(start);
    }

    if(enterInsertMode)
        setMode(Mode::Insert);
    else
        clampNormalCaret();
}

void VimInputHandler::replaceCharacter(const QString& replacement, int count)
{
    if(m_editor->isReadOnly() || replacement.isEmpty())
        return;

    const int start = currentPosition();
    const int lineEnd = lineEndPosition(currentLine());
    int end = start;
    int actualCount = 0;
    while(actualCount < count && end < lineEnd)
    {
        end = positionAfter(end);
        ++actualCount;
    }
    if(actualCount == 0)
        return;

    setSelection(start, end);
    m_editor->replaceSelectedText(replacement.repeated(actualCount));
    setPosition(start);
    for(int i = 1; i < actualCount; ++i)
        setPosition(positionAfter(currentPosition()));
    clampNormalCaret();
}

void VimInputHandler::joinLines(int count, bool raw)
{
    if(m_editor->isReadOnly()) return;
    const int line = currentLine();
    int caret = lineEndPosition(line);
    m_editor->beginUndoAction();
    for(int i = 1; i < std::max(2, count) && line + 1 < m_editor->lines(); ++i)
    {
        int first = lineEndPosition(line), last = positionFromLine(line + 1);
        QString separator;
        if(!raw)
        {
            while(last < lineEndPosition(line + 1) && characterClassAt(last) == 0)
                last = positionAfter(last);
            if(first > positionFromLine(line) && characterClassAt(positionBefore(first)) != 0 &&
               last < lineEndPosition(line + 1) && QString::fromUtf8(m_editor->text().toUtf8().mid(last, positionAfter(last)-last)) != ")")
                separator = " ";
        }
        setSelection(first, last);
        m_editor->replaceSelectedText(separator);
    }
    m_editor->endUndoAction();
    setPosition(caret); clampNormalCaret();
}

void VimInputHandler::toggleCase(int count)
{
    if(m_editor->isReadOnly())
        return;

    const int start = currentPosition();
    const int lineEnd = lineEndPosition(currentLine());
    int end = start;
    for(int i = 0; i < count && end < lineEnd; ++i)
        end = positionAfter(end);
    if(end <= start)
        return;

    setSelection(start, end);
    const QString selected = m_editor->selectedText();
    QString replacement;
    replacement.reserve(selected.size());
    for(const QChar character : selected)
        replacement.append(character.isUpper() ? character.toLower() : character.toUpper());
    m_editor->replaceSelectedText(replacement);
    setPosition(start);
    for(int i = 0; i < count && currentPosition() < lineEndPosition(currentLine()); ++i)
        setPosition(positionAfter(currentPosition()));
    clampNormalCaret();
}

QString VimInputHandler::linesText(int firstLine, int lastLine) const
{
    QString result;
    for(int line = firstLine; line <= lastLine; ++line)
        result += m_editor->text(line);
    if(!result.endsWith('\n') && !result.endsWith('\r'))
        result += endOfLine();
    return result;
}

QString VimInputHandler::endOfLine() const
{
    switch(m_editor->eolMode())
    {
    case QsciScintilla::EolWindows:
        return "\r\n";
    case QsciScintilla::EolMac:
        return "\r";
    case QsciScintilla::EolUnix:
    default:
        return "\n";
    }
}

void VimInputHandler::setRegister(const QString& text, bool linewise, bool yank, bool blockwise)
{
    const QString selected = m_selectedRegister;
    m_selectedRegister.clear();
    if(selected == "_" || text.isEmpty() || (!yank && m_editor->isReadOnly())) return;

    const bool unnamed = selected.isEmpty() || selected == "\"";
    const auto write = [this](const QString& name, const QString& value, bool lines, bool block) {
        m_registers[name] = qMakePair(value, lines);
        m_blockRegisters[name] = block;
    };
    // Explicit named yanks leave register 0 intact; only ordinary yanks replace it.
    if(yank && unnamed) write("0", text, linewise, blockwise);
    if(!yank)
    {
        if(linewise || text.contains('\n') || text.contains('\r'))
        {
            for(int number = 9; number > 1; --number)
            {
                const QString previous = QString::number(number - 1);
                const auto value = m_registers.value(previous);
                write(QString::number(number), value.first, value.second, m_blockRegisters.value(previous));
            }
            write("1", text, linewise, blockwise);
        }
        else if(unnamed)
            write("-", text, false, blockwise);
    }

    QString value = text;
    if(!unnamed)
    {
        const QString name = selected.toLower();
        if(selected != name && m_registers.contains(name))
        {
            const auto previous = m_registers.value(name);
            value = previous.first;
            if(linewise && !value.endsWith('\n') && !value.endsWith('\r')) value += endOfLine();
            value += text;
            linewise = linewise || previous.second;
            if(linewise && !value.endsWith('\n') && !value.endsWith('\r')) value += endOfLine();
            blockwise = !linewise && blockwise && m_blockRegisters.value(name);
        }
        write(name, value, linewise, blockwise);
    }
    m_registerBlock = blockwise;
    m_registerText = value;
    m_registerLinewise = linewise;
    QApplication::clipboard()->setText(value);
    if(selected == "*" && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(value, QClipboard::Selection);
}

QString VimInputHandler::registerText(const QString& name, bool& linewise, bool& blockwise) const
{
    linewise = false;
    blockwise = false;
    if(name == "_") return QString();
    if(!name.isEmpty() && name != "\"" && name != "+" && name != "*")
    {
        const QString key = name.toLower();
        const auto value = m_registers.value(key);
        linewise = value.second;
        blockwise = m_blockRegisters.value(key);
        return value.first;
    }
    const auto clipboardMode = name == "*" && QApplication::clipboard()->supportsSelection()
        ? QClipboard::Selection : QClipboard::Clipboard;
    QString value = QApplication::clipboard()->text(clipboardMode);
    if(value.isEmpty() && (name.isEmpty() || name == "\"")) value = m_registerText;
    if(value == m_registerText)
    {
        linewise = m_registerLinewise;
        blockwise = m_registerBlock;
    }
    return value;
}

// Work in Scintilla columns, splitting a tab only where a rectangle cuts it.
// Return a padded row so a copied short line retains the rectangle's width.
QString VimInputHandler::blockRow(int line, int left, int right, const QString& value, bool replace)
{
    const int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, left));
    int last = first;
    int column = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, first));
    const int initialColumn = column;
    const int lineStart = positionFromLine(line);
    const QByteArray bytes = m_editor->text(line).toUtf8();
    QString removed;
    while(last < lineEndPosition(line) && column < right)
    {
        const int next = positionAfter(last);
        const int nextColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, next));
        if(bytes.at(last - lineStart) == '\t')
            removed += QString(std::max(0, std::min(right, nextColumn) - std::max(left, column)), ' ');
        else if(column >= left)
            removed += QString::fromUtf8(bytes.mid(last - lineStart, next - last));
        last = next;
        column = nextColumn;
    }
    removed += QString(std::max(0, right - std::max(left, column)), ' ');
    if(replace)
    {
        // Insertion inside a tab must split it even for a zero-width range.
        if(first == last && initialColumn < left && first < lineEndPosition(line))
        {
            last = positionAfter(first);
            column = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, last));
        }
        const bool touchesText = last > first;
        const QString prefix(std::max(0, left - initialColumn), ' ');
        const QString suffix(std::max(0, column - right), ' ');
        setSelection(first, last);
        m_editor->replaceSelectedText((touchesText || !value.isEmpty() ? prefix : QString()) + value + suffix);
    }
    return removed;
}

void VimInputHandler::insertBlock(int line, int column, const QStringList& rows, int count)
{
    for(int i = 0; i < rows.size(); ++i)
    {
        while(line + i >= m_editor->lines())
        { setPosition(documentLength()); m_editor->replaceSelectedText(endOfLine()); }
        blockRow(line + i, column, column, rows.at(i).repeated(std::max(1, count)), true);
    }
    setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, column)));
}

void VimInputHandler::paste(bool before, int count)
{
    const QString selected = m_selectedRegister;
    m_selectedRegister.clear();
    if(m_editor->isReadOnly()) return;
    bool linewise = false, blockwise = false;
    QString value = registerText(selected, linewise, blockwise);
    if(value.isEmpty()) return;

    if(blockwise && !linewise)
    {
        const int line = currentLine();
        const int position = !before && currentPosition() < lineEndPosition(line)
            ? positionAfter(currentPosition()) : currentPosition();
        const int column = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, position));
        insertBlock(line, column, value.split('\n'), count);
        return;
    }
    value = value.repeated(std::max(1, count));
    int insertionPosition = currentPosition();
    int caretPosition = insertionPosition;

    if(linewise)
    {
        const int line = currentLine();
        if(before)
        {
            insertionPosition = positionFromLine(line);
            caretPosition = insertionPosition;
        }
        else if(line + 1 < m_editor->lines())
        {
            insertionPosition = positionFromLine(line + 1);
            caretPosition = insertionPosition;
        }
        else
        {
            insertionPosition = documentLength();
            QString eol = endOfLine();
            if(!m_editor->text().endsWith('\n') && !m_editor->text().endsWith('\r'))
            {
                if(value.endsWith(eol))
                    value.chop(eol.length());
                value.prepend(eol);
                caretPosition = insertionPosition + eol.toUtf8().size();
            }
            else
            {
                caretPosition = insertionPosition;
            }
        }
    }
    else if(!before)
    {
        const int lineEnd = lineEndPosition(currentLine());
        if(insertionPosition < lineEnd)
            insertionPosition = positionAfter(insertionPosition);
        caretPosition = insertionPosition;
    }

    setSelection(insertionPosition, insertionPosition);
    m_editor->replaceSelectedText(value);
    setPosition(caretPosition);
    clampNormalCaret();
}

void VimInputHandler::pasteVisual(bool preserveRegisters, int count)
{
    const QString selected = m_selectedRegister;
    m_selectedRegister.clear();
    resetPendingCommand();
    if(m_editor->isReadOnly())
    {
        setPosition(m_visualCaret);
        setMode(Mode::Normal);
        clampNormalCaret();
        return;
    }

    bool linewise = false, blockwise = false;
    // Read before recording the replaced selection: it may overwrite the source
    // unnamed, numbered, small-delete, or clipboard register.
    QString value = registerText(selected, linewise, blockwise);
    if(m_mode == Mode::VisualBlock)
    {
        const int anchorLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualAnchor));
        const int caretLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualCaret));
        const int anchorColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualAnchor));
        const int caretColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualCaret));
        const int firstLine = std::min(anchorLine, caretLine), lastLine = std::max(anchorLine, caretLine);
        const int left = std::min(anchorColumn, caretColumn);
        const int endpoint = anchorColumn > caretColumn ? m_visualAnchor : m_visualCaret;
        const int right = std::max(std::max(anchorColumn, caretColumn) + 1,
            int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, positionAfter(endpoint))));
        QStringList removed;
        setMode(Mode::Normal);
        for(int line = lastLine; line >= firstLine; --line)
            removed.prepend(blockRow(line, left, right, QString(), true));
        if(!preserveRegisters) setRegister(removed.join('\n'), false, false, true);
        value.replace("\r\n", "\n"); value.replace('\r', '\n');
        if(blockwise)
            insertBlock(firstLine, left, value.split('\n'), count);
        else if(!linewise && !value.contains('\n'))
        {
            QStringList rows;
            for(int line = firstLine; line <= lastLine; ++line) rows.append(value);
            insertBlock(firstLine, left, rows, count);
        }
        else
        {
            const int position = linewise ? positionFromLine(firstLine) :
                int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, firstLine, left));
            setPosition(position);
            value.replace("\n", endOfLine());
            m_editor->replaceSelectedText(value.repeated(std::max(1, count)));
            setPosition(position);
        }
        clampNormalCaret();
        return;
    }

    const bool wholeLines = m_mode == Mode::VisualLine;
    const int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
    const int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));
    const QString document = m_editor->text();
    QString replaced = m_editor->selectedText();
    if(wholeLines && !replaced.endsWith('\n') && !replaced.endsWith('\r'))
        replaced += endOfLine();

    // Visual p writes deletion history, whereas modern Vim's P leaves all
    // registers untouched. The selected register is the source, never a target.
    if(!preserveRegisters) setRegister(replaced, wholeLines);

    if(blockwise)
    {
        const int line = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, first));
        const int column = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, first));
        QStringList rows = value.split('\n');
        if(wholeLines)
        {
            for(QString& row : rows) row = row.repeated(std::max(1, count));
            QString replacement = rows.join(endOfLine());
            if(last < documentLength() || document.endsWith(endOfLine())) replacement += endOfLine();
            m_editor->replaceSelectedText(replacement);
            setMode(Mode::Normal); setPosition(first);
        }
        else
        {
            m_editor->replaceSelectedText(QString());
            setMode(Mode::Normal);
            insertBlock(line, column, rows, count);
        }
        clampNormalCaret();
        return;
    }

    value.replace("\r\n", "\n");
    value.replace('\r', '\n');
    value.replace("\n", endOfLine());
    if(wholeLines && !linewise) value += endOfLine();
    value = value.repeated(std::max(1, count));
    int insertedFirst = first;
    if(wholeLines)
    {
        // Keep an unterminated final line unterminated, as normal linewise paste
        // does. Promote a character register before applying the repeat count.
        if(last == documentLength() && !document.endsWith('\n') && !document.endsWith('\r') &&
           value.endsWith(endOfLine())) value.chop(endOfLine().size());
    }
    else if(linewise && !value.isEmpty())
    {
        value.prepend(endOfLine());
        insertedFirst += endOfLine().toUtf8().size();
    }

    // An empty source (including "_) replaces a character selection with nothing;
    // in Visual Line mode it supplies an empty line, matching Vim.
    m_editor->replaceSelectedText(value);
    const int insertedLast = first + value.toUtf8().size();
    m_visualAnchor = insertedFirst;
    m_visualCaret = insertedLast > insertedFirst ? positionBefore(insertedLast) : insertedFirst;
    setMode(Mode::Normal);
    const bool multiline = value.contains('\n') || value.contains('\r');
    setPosition(wholeLines || linewise || multiline || value.isEmpty()
        ? insertedFirst : positionBefore(insertedLast));
    if(wholeLines || linewise) move("^", 1);
    clampNormalCaret();
}

void VimInputHandler::enterVisualMode(bool linewise)
{
    resetPendingCommand();
    m_visualTagSelected = false;
    m_visualAnchor = currentPosition();
    m_visualCaret = m_visualAnchor;
    setMode(linewise ? Mode::VisualLine : Mode::Visual);
    updateVisualSelection();
}

void VimInputHandler::updateVisualSelection()
{
    if(m_mode == Mode::VisualBlock)
    {
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETSELECTIONMODE, QsciScintillaBase::SC_SEL_RECTANGLE);
        int anchorColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualAnchor));
        int caretColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualCaret));
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETRECTANGULARSELECTIONANCHOR,
            anchorColumn > caretColumn ? positionAfter(m_visualAnchor) : m_visualAnchor);
        m_editor->SendScintilla(QsciScintillaBase::SCI_SETRECTANGULARSELECTIONCARET,
            anchorColumn > caretColumn ? m_visualCaret : positionAfter(m_visualCaret));
        return;
    }
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSELECTIONMODE, QsciScintillaBase::SC_SEL_STREAM);
    if(m_mode == Mode::VisualLine)
    {
        const int anchorLine = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualAnchor));
        const int caretLine = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualCaret));
        const int firstLine = std::min(anchorLine, caretLine);
        const int lastLine = std::max(anchorLine, caretLine);
        const int end = lastLine + 1 < m_editor->lines() ? positionFromLine(lastLine + 1) : documentLength();
        setSelection(positionFromLine(firstLine), end);
    }
    else
    {
        const int first = std::min(m_visualAnchor, m_visualCaret);
        const int last = std::max(m_visualAnchor, m_visualCaret);
        setSelection(first, positionAfter(last));
    }
}

void VimInputHandler::finishVisualOperator(const QString& command)
{
    const int start = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
    const bool linewise = m_mode == Mode::VisualLine;

    if(linewise)
    {
        const int anchorLine = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualAnchor));
        const int caretLine = static_cast<int>(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualCaret));
        m_pendingCommand = command;
        applyLineOperator(std::min(anchorLine, caretLine), std::max(anchorLine, caretLine));
        if(command != "c")
            setMode(Mode::Normal);
        resetPendingCommand();
        return;
    }

    setRegister(m_editor->selectedText(), false, command == "y");

    if(command == "y")
    {
        setPosition(start);
        setMode(Mode::Normal);
        clampNormalCaret();
        return;
    }

    if(!m_editor->isReadOnly())
        m_editor->replaceSelectedText(QString());
    setPosition(start);
    setMode(command == "c" ? Mode::Insert : Mode::Normal);
    clampNormalCaret();
}

QVector<QPair<int, int>> VimInputHandler::searchMatches(const QString& pattern) const
{
    QVector<QPair<int, int>> matches;
    if(pattern.isEmpty()) return matches;
    const QRegularExpression expression("(*ANYCRLF)" + pattern, QRegularExpression::MultilineOption |
        QRegularExpression::UseUnicodePropertiesOption);
    if(!expression.isValid()) return matches;
    const QString text = m_editor->text();
    auto iterator = expression.globalMatch(text);
    int previous = 0, bytes = 0;
    while(iterator.hasNext())
    {
        const auto match = iterator.next();
        const int start = match.capturedStart(), end = match.capturedEnd();
        bytes += text.mid(previous, start-previous).toUtf8().size();
        const int size = text.mid(start, end-start).toUtf8().size();
        matches.append(qMakePair(bytes, size));
        previous = start;
    }
    return matches;
}

bool VimInputHandler::moveToSearch(const QString& pattern, bool forward, int start, int count)
{
    const auto matches = searchMatches(pattern);
    if(matches.isEmpty()) return false;
    int index = forward ? 0 : matches.size()-1;
    if(forward)
    {
        while(index < matches.size() && matches.at(index).first <= start) ++index;
        index %= matches.size();
    }
    else
    {
        while(index >= 0 && matches.at(index).first >= start) --index;
        if(index < 0) index = matches.size()-1;
    }
    const int offset = (std::max(1, count)-1) % matches.size();
    index = (index + (forward ? offset : -offset) + matches.size()) % matches.size();
    const int target = matches.at(index).first;
    m_editor->SendScintilla(QsciScintillaBase::SCI_ENSUREVISIBLE,
        m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, target));
    if(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
    { m_visualCaret = target; updateVisualSelection(); }
    else { setPosition(target); clampNormalCaret(); }
    m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SCROLLCARET);
    return true;
}

void VimInputHandler::paintSearch(const QString& pattern)
{
    if(m_searchIndicator < 0 && !pattern.isEmpty())
    {
        m_searchIndicator = m_editor->indicatorDefine(QsciScintilla::StraightBoxIndicator);
        if(m_searchIndicator >= 0)
        {
            m_editor->setIndicatorForegroundColor(QColor(255, 184, 108), m_searchIndicator);
            m_editor->setIndicatorDrawUnder(true, m_searchIndicator);
        }
    }
    if(m_searchIndicator < 0) return;
    const int previous = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETINDICATORCURRENT));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, m_searchIndicator);
    m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0, documentLength());
    for(const auto& match : searchMatches(pattern))
        if(match.second > 0)
            m_editor->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, match.first, match.second);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, previous);
}

void VimInputHandler::promptSearch(bool forward)
{
    if(m_searchActive || m_substituteActive) return;
    m_searchOrigin = (m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock) ?
        m_visualCaret : currentPosition();
    m_searchAnchor = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETANCHOR));
    m_searchTop = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    m_searchX = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET));
    m_searchForward = forward;
    m_searchCount = takeCount();
    m_searchClosedFolds.clear();
    for(int line = 0; line < m_editor->lines(); ++line)
        if((m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDLEVEL, line) & QsciScintillaBase::SC_FOLDLEVELHEADERFLAG) &&
           !m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDEXPANDED, line))
            m_searchClosedFolds.append(line);
    if(!m_searchPrompt)
    {
        m_searchPrompt = new QLineEdit(m_editor);
        m_searchPrompt->setObjectName("vimSearchPrompt");
        m_searchPrompt->installEventFilter(this);
        connect(m_searchPrompt, &QLineEdit::textChanged, this, [this]() { previewSearch(); });
    }
    m_searchPrompt->setPlaceholderText(forward ? tr("/ Search; Enter: accept, Esc: cancel") :
        tr("? Search; Enter: accept, Esc: cancel"));
    m_searchPrompt->clear();
    m_searchDraft.clear();
    m_searchHistoryIndex = m_searchHistory.size();
    m_searchActive = true;
    m_searchPrompt->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
    m_searchPrompt->show(); m_searchPrompt->raise(); m_searchPrompt->setFocus();
}

void VimInputHandler::restoreSearchOrigin()
{
    for(int line : m_searchClosedFolds)
        if(m_editor->SendScintilla(QsciScintillaBase::SCI_GETFOLDEXPANDED, line))
            m_editor->SendScintilla(QsciScintillaBase::SCI_FOLDLINE, line, QsciScintillaBase::SC_FOLDACTION_CONTRACT);
    if(m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock)
    { m_visualCaret = m_searchOrigin; updateVisualSelection(); }
    else setSelection(m_searchAnchor, m_searchOrigin);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, m_searchTop);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, m_searchX);
}

void VimInputHandler::previewSearch()
{
    if(!m_searchActive) return;
    restoreSearchOrigin();
    const QString pattern = m_searchPrompt->text();
    paintSearch(pattern.isEmpty() && m_searchHighlight ? m_lastSearch : pattern);
    const bool valid = QRegularExpression(pattern).isValid();
    const bool found = !pattern.isEmpty() && valid && moveToSearch(pattern, m_searchForward, m_searchOrigin, m_searchCount);
    m_searchPrompt->setToolTip(pattern.isEmpty() ? QString() : !valid ? tr("Invalid regular expression") :
        !found ? tr("Pattern not found") : QString());
}

void VimInputHandler::finishSearch(bool accept, bool restore, bool focus)
{
    if(!m_searchActive) return;
    const QString pattern = m_searchPrompt->text().isEmpty() ? m_lastSearch : m_searchPrompt->text();
    if(accept && !QRegularExpression(pattern).isValid()) return;
    m_searchActive = false;
    if(accept && !pattern.isEmpty())
    {
        m_lastSearch = pattern;
        m_lastSearchForward = m_searchForward;
        m_searchHighlight = true;
        m_searchHistory.removeAll(pattern);
        m_searchHistory.append(pattern);
        if(m_searchHistory.size() > 100) m_searchHistory.removeFirst();
        moveToSearch(pattern, m_searchForward, m_searchOrigin, m_searchCount);
        if(m_mode == Mode::Normal && currentPosition() != m_searchOrigin)
        {
            if(m_jumpIndex+1 < m_jumps.size()) m_jumps.resize(m_jumpIndex+1);
            if(m_jumps.isEmpty() || m_jumps.last() != m_searchOrigin) m_jumps.append(m_searchOrigin);
            m_jumps.append(currentPosition());
            if(m_jumps.size() > 100) m_jumps.removeFirst();
            m_jumpIndex = m_jumps.size()-1;
        }
    }
    else if(restore) restoreSearchOrigin();
    m_searchClosedFolds.clear();
    paintSearch(m_searchHighlight ? m_lastSearch : QString());
    m_searchPrompt->hide();
    if(focus) { m_editor->setFocus(); if(m_replayDepth == 0) finishTemporaryNormal("/"); }
    else m_insertPauses.clear();
}

void VimInputHandler::repeatSearch(bool reverse)
{
    const int count = takeCount();
    if(m_lastSearch.isEmpty()) return;
    m_searchHighlight = true;
    paintSearch(m_lastSearch);
    const int start = (m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock) ?
        m_visualCaret : currentPosition();
    moveToSearch(m_lastSearch, reverse ? !m_lastSearchForward : m_lastSearchForward, start, count);
}

bool VimInputHandler::processStroke(QKeyEvent* event)
{
    const QString key = commandKey(event);
    if(m_mode == Mode::Normal && m_pendingCommand.isEmpty() && key == "q" && !m_recording.isEmpty())
    {
        m_recording.clear();
        if(!m_insertPauses.isEmpty()) finishTemporaryNormal(key);
        return true;
    }
    if(!m_recording.isEmpty() && m_replayDepth == 0)
        m_macros[m_recording].append({event->key(), event->modifiers(), event->text()});
    if(m_sequence.isEmpty()) m_changeBefore = m_editor->text();
    m_sequence.append({event->key(), event->modifiers(), event->text()});
    // Keep an insert/change sequence in one undo unit. Undo/redo themselves must
    // execute outside a group. Replayed commands open their own undo units.
    const bool history = m_mode == Mode::Normal && m_pendingCommand.isEmpty() &&
        (key == "u" || (event->key() == Qt::Key_R && event->modifiers().testFlag(Qt::ControlModifier)));
    if(history && m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    if(!m_groupOpen && !history) { m_editor->beginUndoAction(); m_groupOpen = true; }
    const int beforePosition = currentPosition();
    const bool jump = m_mode == Mode::Normal && (key == "G" || key == "g" || key == "'" || key == "`" || key == "*" || key == "#" || key == "n" || key == "N" || key == "%" || key == "{" || key == "}");
    const bool handled = handleKeyPress(event);
    if(jump && currentPosition() != beforePosition)
    {
        if(m_jumpIndex+1 < m_jumps.size()) m_jumps.resize(m_jumpIndex+1);
        if(m_jumps.isEmpty() || m_jumps.last() != beforePosition) m_jumps.append(beforePosition);
        m_jumps.append(currentPosition());
        if(m_jumps.size() > 100) m_jumps.removeFirst();
        m_jumpIndex = m_jumps.size()-1;
    }
    if(!handled)
    {
        m_forwarding = true;
        QCoreApplication::sendEvent(m_editor, event);
        m_forwarding = false;
    }
    if(m_insertPauses.isEmpty() && m_mode == Mode::Normal && m_pendingCommand.isEmpty() &&
       m_mappingPrefix.isEmpty() && m_count == 0 && m_selectedRegister.isEmpty())
        finishChangeSequence(key, history);
    return true;
}

void VimInputHandler::finishChangeSequence(const QString& key, bool history)
{
    if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    if(!history && m_replayDepth == 0 && m_editor->text() != m_changeBefore)
    {
        if(key != ".") m_lastChange = m_sequence;
        const QByteArray before = m_changeBefore.toUtf8(), after = m_editor->text().toUtf8();
        int changed = 0;
        while(changed < before.size() && changed < after.size() && before.at(changed) == after.at(changed))
            ++changed;
        // A common UTF-8 byte prefix can end inside the changed character.
        while(changed > 0 && changed < after.size() &&
              (static_cast<unsigned char>(after.at(changed)) & 0xc0) == 0x80)
            --changed;
        if(m_changes.isEmpty() || m_changes.last() != changed) m_changes.append(changed);
        if(m_changes.size() > 100) m_changes.removeFirst();
        m_changeIndex = m_changes.size();
    }
    m_sequence.clear();
    if(key != "\"") m_selectedRegister.clear();
}

void VimInputHandler::replay(const Strokes& input, int count)
{
    if(m_replayDepth >= 10 || input.size() > 10000) return;
    const Strokes keys = input;
    if(m_replayDepth == 0) m_replayBudget = 10000;
    ++m_replayDepth;
    for(int n = 0; n < std::min(count, 10000 / std::max(1, keys.size())); ++n)
        for(const Stroke& stroke : keys)
        {
            if(m_replayBudget-- <= 0) { --m_replayDepth; return; }
            if(stroke.completionDeleteBytes >= 0)
            {
                resetInsertCompletion();
                if(m_mode == Mode::Insert && !m_insertRegisterPending && !m_editor->isReadOnly() &&
                   !m_editor->hasSelectedText() && stroke.completionDeleteBytes <= currentPosition())
                {
                    flushInsertMappingPrefix();
                    const int caret = currentPosition();
                    setSelection(caret - stroke.completionDeleteBytes, caret);
                    m_editor->replaceSelectedText(stroke.completionText);
                }
                continue;
            }
            QKeyEvent event(QEvent::KeyPress, stroke.key, stroke.modifiers, stroke.text);
            if(!handleKeyPress(&event))
            {
                m_forwarding = true; QCoreApplication::sendEvent(m_editor, &event); m_forwarding = false;
            }
        }
    --m_replayDepth;
}

bool VimInputHandler::extendedNormal(const QString& key)
{
    if(key == ":") { promptCommand(); return true; }
    if(key == ".") { replay(m_lastChange, takeCount()); return true; }
    if(key == "q" || key == "@" || key == "\"" || key == "m" || key == "'" || key == "`" ||
       key == ">" || key == "<" || key == "=" || (key.size() == 1 && QString("fFtT").contains(key)))
    { m_pendingCommand = key; m_pendingCount = takeCount(); return true; }
    if(key == ";" || key == ",")
    {
        repeatFindMotion(key == ",", takeCount());
        return true;
    }
    if(key == "X")
    {
        int last = currentPosition(), first = last, count = takeCount();
        while(count-- && first > positionFromLine(currentLine())) first = positionBefore(first);
        m_pendingCommand = "d"; applyCharacterOperator(first, last); resetPendingCommand(); return true;
    }
    if(key == "S")
    { m_pendingCommand = "c"; int count = takeCount(); applyLineOperator(currentLine(), currentLine()+count-1); resetPendingCommand(); return true; }
    if(key == "R") { m_replace = true; m_editor->setOverwriteMode(true); setMode(Mode::Insert); return true; }
    if(key == "*" || key == "#")
    {
        int first, last;
        if(textObjectRange("w", false, 1, first, last))
        {
            QString word = QString::fromUtf8(m_editor->text().toUtf8().mid(first, last-first));
            m_lastSearch = "\\b" + QRegularExpression::escape(word) + "\\b";
            m_lastSearchForward = key == "*"; repeatSearch(false);
        }
        takeCount(); return true;
    }
    return false;
}

bool VimInputHandler::extendedPending(const QString& key)
{
    const QString pending = m_pendingCommand;
    if(handleGMotion(key)) return true;
    if(pending == "q")
    {
        resetPendingCommand();
        if(key.size() == 1 && key.at(0).isLetter()) { m_recording = key.toLower(); m_macros[m_recording].clear(); }
        return true;
    }
    if(pending == "@")
    {
        int count = m_pendingCount * takeCount(); resetPendingCommand();
        QString name = key == "@" ? m_lastMacro : key.toLower();
        if(m_macros.contains(name)) { m_lastMacro = name; replay(m_macros.value(name), count); }
        return true;
    }
    if(pending == "\"")
    {
        if(!isRegisterName(key)) { m_selectedRegister.clear(); resetPendingCommand(); return true; }
        m_selectedRegister = key; m_pendingCommand.clear();
        // Keep a register prefix and its following operation in the same sequence.
        m_count = m_pendingCount == 1 ? 0 : m_pendingCount; return true;
    }
    if(pending == "m" || pending == "'" || pending == "`")
    {
        if(pending == "m") m_marks[key] = currentPosition();
        else if(m_marks.contains(key))
        {
            int previous = currentPosition(); setPosition(m_marks.value(key));
            if(pending == "'") move("^", 1);
            m_marks["'"] = previous; m_marks["`"] = previous;
        }
        resetPendingCommand(); return true;
    }
    if(!pending.isEmpty() && QString("fFtT").contains(pending.right(1)))
    {
        const QString command = pending.right(1);
        const QString operation = pending.left(pending.size()-1);
        applyFindMotion(command, key, m_pendingCount * takeCount(), false, operation);
        m_findCommand = command; m_findTarget = key;
        if(!operation.isEmpty()) m_selectedRegister.clear();
        resetPendingCommand(); return true;
    }
    if(pending == "g" && (key == ";" || key == ","))
    {
        const int count = m_pendingCount * takeCount();
        if(!m_changes.isEmpty())
        {
            m_changeIndex = std::max(0, std::min(m_changes.size() - 1,
                m_changeIndex + (key == ";" ? -count : count)));
            setPosition(m_changes.at(m_changeIndex));
            clampNormalCaret();
        }
        resetPendingCommand(); return true;
    }
    if(pending == "g" && (key == "u" || key == "U" || key == "~"))
    { m_pendingCommand += key; return true; }
    if(pending == "g" && key == "J") { int count = m_pendingCount; resetPendingCommand(); joinLines(count, true); return true; }
    if(pending == "g" && key == "v")
    {
        resetPendingCommand(); setMode(m_savedVisualMode);
        m_visualAnchor = std::min(m_savedAnchor, documentLength()); m_visualCaret = std::min(m_savedCaret, documentLength());
        updateVisualSelection(); return true;
    }
    if((pending == "gu" || pending == "gU" || pending == "g~") && key == pending.right(1))
    { applyLineOperator(currentLine(), currentLine()+m_pendingCount*takeCount()-1); resetPendingCommand(); return true; }
    if((pending == "gug" || pending == "gUg" || pending == "g~g") && key == pending.mid(1, 1))
    {
        m_pendingCommand.chop(1);
        applyLineOperator(currentLine(), currentLine()+m_pendingCount*takeCount()-1);
        resetPendingCommand();
        return true;
    }
    return false;
}

bool VimInputHandler::handleGMotion(const QString& key)
{
    if(!m_pendingCommand.endsWith("g") || (key != "g" && key != "_" && key != "e" && key != "E"))
        return false;
    const QString operation = m_pendingCommand.left(m_pendingCommand.size()-1);
    if(!operation.isEmpty() && !isMotionOperator(operation)) return false;
    const int count = m_pendingCount * takeCount();
    const bool visual = m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock;
    if(visual) setPosition(m_visualCaret);
    if(operation.isEmpty())
        move("g" + key, count);
    else
    {
        m_pendingCommand = operation;
        applyOperatorMotion("g" + key, count);
        m_selectedRegister.clear();
    }
    if(visual)
    {
        m_visualCaret = currentPosition();
        updateVisualSelection();
    }
    resetPendingCommand();
    return true;
}

bool VimInputHandler::repeatFindMotion(bool reverse, int count, const QString& operation)
{
    QString command = m_findCommand;
    if(reverse && !command.isEmpty())
        command = command == command.toUpper() ? command.toLower() : command.toUpper();
    return applyFindMotion(command, m_findTarget, count, true, operation);
}

bool VimInputHandler::applyFindMotion(const QString& command, const QString& target, int count,
                                     bool repeat, const QString& operation)
{
    const bool visual = m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock;
    const int start = visual ? m_visualCaret : currentPosition();
    if(visual) setPosition(start);
    bool withinLine = true;
    if(repeat && command.toLower() == "t")
    {
        // Repeating t/T must skip the adjacent previously found character, without
        // crossing an EOL or walking into a different line when no match remains.
        const int next = command == "t" ? positionAfter(start) : positionBefore(start);
        withinLine = next != start && next >= positionFromLine(currentLine()) && next < lineEndPosition(currentLine());
        if(withinLine) setPosition(next);
    }
    const bool found = withinLine && findCharacter(command, target, count);
    if(!found) setPosition(start);
    if(visual)
    {
        m_visualCaret = currentPosition();
        updateVisualSelection();
    }
    else if(found && !operation.isEmpty())
    {
        const int finish = currentPosition();
        m_pendingCommand = operation;
        // Forward f/t are inclusive; backward F/T exclude the original cursor.
        // T may legitimately end where it started, which is an empty operator range.
        if(command == command.toLower()) applyCharacterOperator(start, positionAfter(finish));
        else applyCharacterOperator(finish, start);
    }
    return found;
}

bool VimInputHandler::findCharacter(const QString& command, const QString& target, int count)
{
    if(command.isEmpty() || target.isEmpty()) return false;
    bool forward = command == command.toLower();
    int position = currentPosition(), first = positionFromLine(currentLine()), last = lineEndPosition(currentLine());
    const QByteArray bytes = m_editor->text().toUtf8();
    for(int i = 0; i < count; ++i)
    {
        bool found = false;
        while(forward ? position < last : position > first)
        {
            position = forward ? positionAfter(position) : positionBefore(position);
            if(position < last && QString::fromUtf8(bytes.mid(position, positionAfter(position)-position)) == target)
            { found = true; break; }
        }
        if(!found) return false;
    }
    if(command.toLower() == "t") position = forward ? positionBefore(position) : positionAfter(position);
    setPosition(position);
    m_editor->SendScintilla(QsciScintillaBase::SCI_CHOOSECARETX);
    return true;
}

void VimInputHandler::transformRange(int first, int last, const QString& operation)
{
    if(m_editor->isReadOnly()) return;
    setSelection(first, last); QString value = m_editor->selectedText();
    if(operation == "u") value = value.toLower();
    else if(operation == "U") value = value.toUpper();
    else for(int i = 0; i < value.size(); ++i) value[i] = value.at(i).isUpper() ? value.at(i).toLower() : value.at(i).toUpper();
    m_editor->replaceSelectedText(value); setPosition(first); clampNormalCaret();
}

void VimInputHandler::indentLines(int first, int last, const QString& operation)
{
    if(m_editor->isReadOnly()) return;
    int width = m_editor->indentationWidth(); if(width <= 0) width = m_editor->tabWidth();
    m_editor->beginUndoAction();
    for(int line = first; line <= last; ++line)
    {
        int indentation = m_editor->indentation(line);
        if(operation == ">") indentation += width;
        else if(operation == "<") indentation = std::max(0, indentation-width);
        else indentation = line ? m_editor->indentation(line-1) : 0;
        m_editor->setIndentation(line, indentation);
    }
    m_editor->endUndoAction(); setPosition(positionFromLine(first)); move("^", 1);
}

bool VimInputHandler::loadConfig(const QString& path)
{
    QFile file(path); if(!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if(error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject config = document.object();
    QString leader = config.value("leader").toString(",");
    if(leader.size() == 1) m_leader = leader;
    m_mappingTimer->setInterval(std::max(100, std::min(5000, config.value("timeoutMs").toInt(700))));
    if(config.contains("shiftWidth")) m_editor->setIndentationWidth(std::max(1, std::min(16, config.value("shiftWidth").toInt(4))));
    m_userMappings.clear();
    const auto mappings = config.value("mappings").toObject();
    for(auto it = mappings.begin(); it != mappings.end(); ++it)
        if(it.value().isString()) m_userMappings[it.key()] = it.value().toString();
    return true;
}

void VimInputHandler::playMapping(const QString& mapping)
{
    Strokes strokes;
    for(int i = 0; i < mapping.size(); ++i)
    {
        if(mapping.mid(i).startsWith("<Esc>", Qt::CaseInsensitive))
        { strokes.append({Qt::Key_Escape, Qt::NoModifier, QString()}); i += 4; }
        else if(mapping.mid(i).startsWith("<CR>", Qt::CaseInsensitive))
        { strokes.append({Qt::Key_Return, Qt::NoModifier, "\r"}); i += 3; }
        else
        {
            QChar c = mapping.at(i);
            strokes.append({c.toUpper().unicode(), c.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, QString(c)});
        }
    }
    replay(strokes, 1);
}

void VimInputHandler::promptCommand()
{
    const bool visualRange = m_mode == Mode::Visual || m_mode == Mode::VisualLine || m_mode == Mode::VisualBlock;
    if(visualRange) { setMode(Mode::Normal); setPosition(m_visualCaret); }
    if(!m_commandLine)
    {
        m_commandLine = new QLineEdit(m_editor);
        m_commandLine->installEventFilter(this);
        m_commandLine->setPlaceholderText(QString::fromUtf8("輸入指令，例如 %s/foo/bar/g；Esc 取消"));
        connect(m_commandLine, &QLineEdit::returnPressed, this, [this]() {
            const QString command = m_commandLine->text();
            if(executeCommand(command))
            {
                m_commandLine->hide();
                if(!m_substituteActive)
                {
                    m_editor->setFocus();
                    finishTemporaryNormal(":");
                }
            }
            else m_commandLine->setToolTip(QString::fromUtf8("指令不支援、格式錯誤或找不到符合項目"));
        });
        auto* cancel = new QAction(m_commandLine);
        cancel->setShortcut(QKeySequence(Qt::Key_Escape)); cancel->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_commandLine->addAction(cancel);
        connect(cancel, &QAction::triggered, this, [this]() {
            m_commandLine->hide(); m_editor->setFocus(); finishTemporaryNormal(":");
        });
    }
    m_commandLine->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
    m_commandLine->setText(visualRange ? "'<,'>" : QString());
    m_commandLine->show(); m_commandLine->raise(); m_commandLine->setFocus();
}

bool VimInputHandler::executeCommand(const QString& input)
{
    if(m_substituteActive || m_searchActive) return false;
    QString command = input.trimmed();
    if(command.startsWith(':')) command = command.mid(1).trimmed();
    if(command == "w" || command == "write")
    {
        if(auto* action = m_editor->window()->findChild<QAction*>("actionSqlSaveFile")) { action->trigger(); return true; }
        return false;
    }
    if(command == "noh" || command == "nohlsearch") { m_searchHighlight = false; paintSearch(QString()); return true; }

    // Scintilla exposes an extra empty line after a final EOL. Ex addresses,
    // unlike editor cursor positions, refer to actual buffer lines only.
    int lineCount = m_editor->lines();
    if(lineCount > 1 && positionFromLine(lineCount - 1) == documentLength()) --lineCount;
    const int cursorLine = std::min(currentLine(), lineCount - 1);
    int firstLine = cursorLine, lastLine = cursorLine;
    bool hasRange = false, rangePair = false;
    int offset = 0;
    const auto skipSpace = [&]() {
        while(offset < command.size() && command.at(offset).isSpace()) ++offset;
    };
    int addressCurrent = cursorLine;
    const auto parseAddress = [&](int relativeLine, int& result, bool allowZero) {
        skipSpace();
        if(offset == command.size()) return false;
        qint64 value = relativeLine;
        const QChar initial = command.at(offset);
        if(initial.isDigit())
        {
            const int start = offset;
            while(offset < command.size() && command.at(offset).isDigit()) ++offset;
            bool valid = false;
            value = command.mid(start, offset - start).toLongLong(&valid);
            if(!valid || value < (allowZero ? 0 : 1) || value > std::numeric_limits<int>::max()) return false;
            --value;
        }
        else if(initial == '.') ++offset;
        else if(initial == '$') { value = lineCount - 1; ++offset; }
        else if(initial == '\'')
        {
            if(++offset == command.size()) return false;
            const QString mark = command.mid(offset++, 1);
            if(!m_marks.contains(mark)) return false;
            value = m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_marks.value(mark));
            value = std::min(value, qint64(lineCount - 1));
        }
        else if(initial != '+' && initial != '-') return false;
        skipSpace();
        while(offset < command.size() && (command.at(offset) == '+' || command.at(offset) == '-'))
        {
            const bool subtract = command.at(offset++) == '-';
            skipSpace();
            const int start = offset;
            while(offset < command.size() && command.at(offset).isDigit()) ++offset;
            qint64 amount = 1;
            if(offset != start)
            {
                bool valid = false;
                amount = command.mid(start, offset - start).toLongLong(&valid);
                if(!valid || amount > std::numeric_limits<int>::max()) return false;
            }
            value += subtract ? -amount : amount;
            if(value < -qint64(std::numeric_limits<int>::max()) || value > std::numeric_limits<int>::max()) return false;
            skipSpace();
        }
        if(value < (allowZero ? -1 : 0) || value >= lineCount) return false;
        result = int(value);
        return true;
    };
    if(command.startsWith('%'))
    {
        firstLine = 0; lastLine = lineCount - 1; hasRange = true; rangePair = true; ++offset;
    }
    else if(!command.isEmpty() && (command.at(0).isDigit() || QString(".$'+-").contains(command.at(0))))
    {
        hasRange = true;
        if(!parseAddress(cursorLine, firstLine, false)) return false;
        lastLine = firstLine;
        if(offset < command.size() && (command.at(offset) == ',' || command.at(offset) == ';'))
        {
            rangePair = true;
            const bool relativeToFirst = command.at(offset++) == ';';
            if(relativeToFirst) addressCurrent = firstLine;
            if(!parseAddress(addressCurrent, lastLine, false)) return false;
        }
    }
    if(lastLine < firstLine) return false;
    command = command.mid(offset).trimmed();
    if(command.isEmpty())
    {
        if(!hasRange) return false;
        setPosition(positionFromLine(lastLine)); move("^", 1); return true;
    }
    if(command == "d" || command == "delete" || command == "y" || command == "yank")
    {
        if(command.startsWith('d') && m_editor->isReadOnly()) return false;
        m_pendingCommand = command.left(1);
        applyLineOperator(firstLine, lastLine);
        resetPendingCommand();
        return true;
    }
    const auto transfer = QRegularExpression("^(co(?:p(?:y)?)?|t|m(?:o(?:v(?:e)?)?)?)(?=$|[^A-Za-z])(.*)$").match(command);
    if(transfer.hasMatch())
    {
        if(m_editor->isReadOnly()) return false;
        const bool moving = transfer.captured(1).startsWith('m');
        command = transfer.captured(2); offset = 0;
        int destination = 0;
        if(!parseAddress(addressCurrent, destination, true)) return false;
        skipSpace(); if(offset != command.size()) return false;
        if(moving && destination >= firstLine && destination < lastLine) return false;
        const int size = lastLine - firstLine + 1;
        if(moving && (destination == lastLine || destination == firstLine - 1)) return true;
        const QByteArray bytes = m_editor->text().toUtf8();
        QStringList original;
        for(int line = 0; line < lineCount; ++line)
            original.append(QString::fromUtf8(bytes.mid(positionFromLine(line), lineEndPosition(line) - positionFromLine(line))));
        QStringList rows = original;
        const QStringList selected = original.mid(firstLine, size);
        int insertion = destination + 1;
        if(moving)
        {
            for(int i = 0; i < size; ++i) rows.removeAt(firstLine);
            if(destination > lastLine) insertion -= size;
        }
        for(int i = 0; i < size; ++i) rows.insert(insertion + i, selected.at(i));
        // Replace only the affected line interval; preserve the rest of the
        // document and its final-EOL presence. Registers are not involved.
        int prefix = 0, suffix = 0;
        while(prefix < original.size() && prefix < rows.size() && original.at(prefix) == rows.at(prefix)) ++prefix;
        while(suffix < original.size() - prefix && suffix < rows.size() - prefix &&
              original.at(original.size()-1-suffix) == rows.at(rows.size()-1-suffix)) ++suffix;
        if(prefix != original.size() || prefix != rows.size())
        {
            const bool finalEol = lineEndPosition(lineCount - 1) < documentLength();
            int first = prefix < lineCount ? positionFromLine(prefix) : documentLength();
            const int last = suffix ? positionFromLine(lineCount - suffix) : documentLength();
            QString replacement = rows.mid(prefix, rows.size()-prefix-suffix).join(endOfLine());
            if(suffix || finalEol) replacement += endOfLine();
            if(prefix == lineCount && !finalEol) replacement.prepend(endOfLine());
            m_editor->beginUndoAction();
            setSelection(first, last); m_editor->replaceSelectedText(replacement);
            m_editor->endUndoAction();
        }
        setPosition(positionFromLine(insertion + size - 1)); move("^", 1);
        return true;
    }
    const auto join = QRegularExpression("^j(?:o(?:i(?:n)?)?)?(!)?(?:\\s+([0-9]+))?$").match(command);
    if(join.hasMatch())
    {
        if(m_editor->isReadOnly()) return false;
        if(!join.captured(2).isEmpty())
        {
            bool valid = false;
            const qint64 count = join.captured(2).toLongLong(&valid);
            if(!valid || count < 1 || count > std::numeric_limits<int>::max()) return false;
            firstLine = lastLine;
            if(qint64(firstLine) + count > lineCount) return false;
            lastLine = firstLine + int(count) - 1;
        }
        else if(!rangePair)
        {
            if(!hasRange && firstLine + 1 >= lineCount) return false;
            lastLine = std::min(firstLine + 1, lineCount - 1);
        }
        if(lastLine == firstLine) return true;
        setPosition(positionFromLine(firstLine));
        joinLines(lastLine - firstLine + 1, !join.captured(1).isEmpty());
        return true;
    }
    const auto sort = QRegularExpression("^sor(?:t)?(!)?(?:\\s+(u))?$").match(command);
    if(sort.hasMatch())
    {
        if(m_editor->isReadOnly()) return false;
        if(!hasRange) { firstLine = 0; lastLine = lineCount - 1; }
        const QByteArray bytes = m_editor->text().toUtf8();
        QStringList rows, endings;
        for(int line = firstLine; line <= lastLine; ++line)
        {
            const int first = positionFromLine(line), last = lineEndPosition(line);
            const int after = line + 1 < m_editor->lines() ? positionFromLine(line + 1) : documentLength();
            rows.append(QString::fromUtf8(bytes.mid(first, last - first)));
            endings.append(QString::fromUtf8(bytes.mid(last, after - last)));
        }
        std::sort(rows.begin(), rows.end(), [](const QString& left, const QString& right) {
            return QString::compare(left, right, Qt::CaseSensitive) < 0;
        });
        if(!sort.captured(2).isEmpty()) rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        if(!sort.captured(1).isEmpty()) std::reverse(rows.begin(), rows.end());
        QString replacement;
        for(int row = 0; row < rows.size(); ++row)
        {
            replacement += rows.at(row);
            // Preserve the original boundary EOLs (including CRLF and the
            // final EOL's presence), even when unique removes some rows.
            if(row + 1 == rows.size()) replacement += endings.last();
            else replacement += endings.at(row).isEmpty() ? endOfLine() : endings.at(row);
        }
        const int first = positionFromLine(firstLine);
        const int last = lastLine + 1 < m_editor->lines() ? positionFromLine(lastLine + 1) : documentLength();
        if(replacement != QString::fromUtf8(bytes.mid(first, last - first)))
        {
            m_editor->beginUndoAction();
            setSelection(first, last); m_editor->replaceSelectedText(replacement);
            m_editor->endUndoAction();
        }
        setPosition(first); move("^", 1); return true;
    }
    // Delimiters can be escaped. The regular-expression dialect is Qt/PCRE,
    // intentionally documented rather than silently pretending to be Vimscript.
    if(command.startsWith("g/") || command.startsWith("v/"))
    {
        const int end = command.lastIndexOf('/');
        if(end <= 1 || command.mid(end+1) != "d" || m_editor->isReadOnly()) return false;
        QRegularExpression expression(command.mid(2, end-2)); if(!expression.isValid()) return false;
        if(!hasRange) { firstLine = 0; lastLine = lineCount - 1; }
        QVector<int> lines;
        for(int n = firstLine; n <= lastLine; ++n)
            if(expression.match(m_editor->text(n)).hasMatch() != command.startsWith("v/")) lines.append(n);
        if(lines.isEmpty()) return false;
        m_editor->beginUndoAction();
        m_pendingCommand = "d";
        for(int i = lines.size()-1; i >= 0; --i) applyLineOperator(lines.at(i), lines.at(i));
        resetPendingCommand(); m_editor->endUndoAction(); return true;
    }
    QStringList parts;
    QString optionText;
    const auto repeat = QRegularExpression("^(?:substitute|substitut|substitu|substit|substi|subst|subs|sub|su|s|&)\\s*([&gic]*)(?:\\s+([0-9]+))?$").match(command);
    // Bare :s and :& repeat the pattern/replacement, not the old flags.
    if(repeat.hasMatch())
    {
        if(!m_haveSubstitute) return false;
        parts << m_substitutePattern << m_substituteReplacement;
        optionText = repeat.captured(1);
        if(!repeat.captured(2).isEmpty()) optionText += " " + repeat.captured(2);
    }
    else
    {
        const auto name = QRegularExpression("^(substitute|substitut|substitu|substit|substi|subst|subs|sub|su|s)").match(command);
        if(!name.hasMatch()) return false;
        command = "s" + command.mid(name.capturedLength());
        if(command.size() < 2) return false;
        const QChar delimiter = command.at(1);
        if(delimiter.isLetterOrNumber() || delimiter.isSpace() || delimiter == '\\' || delimiter == '|') return false;
        QString part;
        for(int i = 2; i < command.size(); ++i)
        {
            if(command.at(i) == '\\')
            {
                if(i+1 == command.size()) return false;
                if(command.at(i+1) == delimiter)
                {
                    if(parts.size() == 1 && delimiter == '&') part += command.at(i);
                    part += delimiter;
                }
                else { part += command.at(i); part += command.at(i+1); }
                ++i;
            }
            else if(command.at(i) == delimiter) { parts << part; part.clear(); }
            else part += command.at(i);
        }
        parts << part;
        if(parts.size() < 2 || parts.size() > 3) return false;
        optionText = parts.size() == 3 ? parts.at(2) : QString();
        if(parts.at(0).isEmpty())
        {
            if(m_lastSearch.isEmpty()) return false;
            parts[0] = m_lastSearch;
        }
    }
    const auto options = QRegularExpression("^(&?[gic]*)(?:\\s+([0-9]+))?$").match(optionText);
    if(!options.hasMatch()) return false;
    QString flags = options.captured(1);
    if(flags.startsWith('&')) flags = m_substituteFlags + flags.mid(1);
    if(!options.captured(2).isEmpty())
    {
        bool valid = false;
        const qint64 count = options.captured(2).toLongLong(&valid);
        if(!valid || count < 1 || count > std::numeric_limits<int>::max()) return false;
        firstLine = lastLine;
        lastLine = int(std::min(qint64(lineCount-1), qint64(firstLine)+count-1));
    }
    QRegularExpression expression(parts.at(0), flags.contains('i') ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    if(!expression.isValid() || m_editor->isReadOnly()) return false;
    if(flags.contains('c') && (!m_enabled || m_mode == Mode::Insert)) return false;
    m_haveSubstitute = true;
    m_substitutePattern = parts.at(0); m_substituteReplacement = parts.at(1); m_substituteFlags = flags;
    m_lastSearch = parts.at(0);
    if(m_searchHighlight) paintSearch(m_lastSearch);
    const auto replacementFor = [&parts](const QRegularExpressionMatch& match) {
        QString replacement;
        const QString pattern = parts.at(1);
        // Interpret the template once: captured text is literal, even if it
        // contains '&' or backslashes that resemble another backreference.
        for(int i = 0; i < pattern.size(); ++i)
        {
            const QChar c = pattern.at(i);
            if(c == '&') replacement += match.captured();
            else if(c == '\\' && i + 1 < pattern.size())
            {
                const QChar next = pattern.at(++i);
                if(next >= '0' && next <= '9') replacement += match.captured(next.digitValue());
                else if(next == '&' || next == '\\') replacement += next;
                else { replacement += c; replacement += next; }
            }
            else replacement += c;
        }
        return replacement;
    };
    if(flags.contains('c'))
    {
        if(!m_enabled || m_mode == Mode::Insert) return false;
        const QByteArray bytes = m_editor->text().toUtf8();
        QVector<SubstituteMatch> matches;
        for(int line = firstLine; line <= lastLine; ++line)
        {
            const int first = positionFromLine(line), last = lineEndPosition(line);
            const QString value = QString::fromUtf8(bytes.mid(first, last-first));
            auto found = expression.globalMatch(value);
            while(found.hasNext())
            {
                const auto match = found.next();
                const int start = first + value.left(match.capturedStart()).toUtf8().size();
                const int end = start + match.captured().toUtf8().size();
                matches.append({start, end, replacementFor(match)});
                if(!flags.contains('g')) break;
            }
        }
        return beginSubstituteConfirmation(matches);
    }
    bool changed = false; m_editor->beginUndoAction();
    for(int n = lastLine; n >= firstLine; --n)
    {
        int first = positionFromLine(n), last = lineEndPosition(n);
        QString value = QString::fromUtf8(m_editor->text().toUtf8().mid(first, last-first));
        auto matches = expression.globalMatch(value); QVector<QRegularExpressionMatch> found;
        while(matches.hasNext()) { found << matches.next(); if(!flags.contains('g')) break; }
        for(int i = found.size()-1; i >= 0; --i)
        {
            const auto match = found.at(i);
            value.replace(match.capturedStart(), match.capturedLength(), replacementFor(match)); changed = true;
        }
        if(!found.isEmpty()) { setSelection(first, last); m_editor->replaceSelectedText(value); }
    }
    m_editor->endUndoAction(); setPosition(positionFromLine(firstLine)); clampNormalCaret(); return changed;
}

bool VimInputHandler::beginSubstituteConfirmation(const QVector<SubstituteMatch>& matches)
{
    if(matches.isEmpty()) return false;
    if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    m_sequence.clear(); resetPendingCommand();
    setMode(Mode::Normal);
    m_substituteMatches = matches;
    m_substituteExpected = m_editor->text().toUtf8();
    m_substituteIndex = 0; m_substituteOffset = 0;
    m_substituteActive = true;
    if(!m_substitutePrompt)
    {
        m_substitutePrompt = new QLineEdit(m_editor);
        m_substitutePrompt->setObjectName("vimSubstituteConfirmation");
        m_substitutePrompt->setReadOnly(true);
        m_substitutePrompt->installEventFilter(this);
    }
    m_substitutePrompt->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
    showSubstituteMatch();
    return true;
}

void VimInputHandler::showSubstituteMatch()
{
    if(!m_substituteActive) return;
    if(m_substituteIndex >= m_substituteMatches.size()) { finishSubstituteConfirmation(); return; }
    const auto match = m_substituteMatches.at(m_substituteIndex);
    setSelection(match.first + m_substituteOffset, match.last + m_substituteOffset);
    if(!m_substituteActive) return;
    m_editor->SendScintilla(QsciScintillaBase::SCI_SCROLLCARET);
    QString replacement = match.replacement;
    replacement.replace('\r', "\\r"); replacement.replace('\n', "\\n");
    if(replacement.size() > 60) replacement = replacement.left(57) + "...";
    const QString prompt = tr("%1/%2  y:yes n:no a:all q:quit l:last Esc:cancel  -> %3")
        .arg(m_substituteIndex + 1).arg(m_substituteMatches.size()).arg(replacement);
    m_substitutePrompt->setText(prompt);
    m_substitutePrompt->setToolTip(prompt);
    m_substitutePrompt->setCursorPosition(0);
    m_substitutePrompt->show(); m_substitutePrompt->raise(); m_substitutePrompt->setFocus();
}

bool VimInputHandler::acceptSubstituteMatch()
{
    if(!m_substituteActive || m_substituteIndex >= m_substituteMatches.size()) return false;
    if(m_editor->isReadOnly() || m_editor->text().toUtf8() != m_substituteExpected)
    { finishSubstituteConfirmation(); return false; }
    const auto match = m_substituteMatches.at(m_substituteIndex);
    const int first = match.first + m_substituteOffset, last = match.last + m_substituteOffset;
    const QByteArray replacement = match.replacement.toUtf8();
    if(m_substituteExpected.mid(first, last-first) != replacement)
    {
        if(!m_substituteUndoOpen) { m_editor->beginUndoAction(); m_substituteUndoOpen = true; }
        m_substituteChanging = true;
        setSelection(first, last); m_editor->replaceSelectedText(match.replacement);
        m_substituteChanging = false;
        if(!m_substituteActive) return false;
        m_substituteExpected.replace(first, last-first, replacement);
        if(m_editor->text().toUtf8() != m_substituteExpected)
        { finishSubstituteConfirmation(); return false; }
    }
    m_substituteOffset += replacement.size() - (last-first);
    ++m_substituteIndex;
    return true;
}

bool VimInputHandler::handleSubstituteConfirmation(QKeyEvent* event)
{
    if(!m_enabled || m_editor->isReadOnly() || m_editor->text().toUtf8() != m_substituteExpected)
    { finishSubstituteConfirmation(); return true; }
    const bool escape = event->key() == Qt::Key_Escape ||
        (event->key() == Qt::Key_BracketLeft && event->modifiers() == Qt::ControlModifier);
    if(escape) { finishSubstituteConfirmation(); return true; }
    if(event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::AltModifier) ||
       event->modifiers().testFlag(Qt::MetaModifier)) return true;
    const QString key = commandKey(event);
    if(key == "q") { finishSubstituteConfirmation(); return true; }
    if(key == "n") ++m_substituteIndex;
    else if(key == "a")
    {
        while(m_substituteActive && m_substituteIndex < m_substituteMatches.size())
            if(!acceptSubstituteMatch()) break;
    }
    else if(key == "y" || key == "l")
    {
        if(!acceptSubstituteMatch()) return true;
        if(key == "l") { finishSubstituteConfirmation(); return true; }
    }
    else return true;
    showSubstituteMatch();
    return true;
}

void VimInputHandler::finishSubstituteConfirmation(bool restoreFocus)
{
    if(!m_substituteActive) return;
    m_substituteActive = false;
    if(m_substituteUndoOpen) { m_editor->endUndoAction(); m_substituteUndoOpen = false; }
    m_substituteMatches.clear(); m_substituteExpected.clear();
    m_substituteIndex = 0; m_substituteOffset = 0;
    if(m_substitutePrompt) m_substitutePrompt->hide();
    if(restoreFocus)
    {
        setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART)));
        clampNormalCaret(); m_editor->setFocus();
        finishTemporaryNormal(":");
    }
    else
    {
        // External edits, input methods, and mouse actions invalidate the saved
        // CTRL-O insertion context. Stay Normal without moving the host caret.
        m_insertPauses.clear();
    }
}

void VimInputHandler::finishBlockOperator(const QString& command)
{
    const int anchorLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualAnchor));
    const int caretLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualCaret));
    const int anchorColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualAnchor));
    const int caretColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualCaret));
    m_blockFirst = std::min(anchorLine, caretLine); m_blockLast = std::max(anchorLine, caretLine);
    m_blockColumn = std::min(anchorColumn, caretColumn);
    const int endpoint = anchorColumn > caretColumn ? m_visualAnchor : m_visualCaret;
    const int lastColumn = std::max(std::max(anchorColumn, caretColumn) + 1,
        int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, positionAfter(endpoint))));
    QStringList rows;
    for(int line = m_blockFirst; line <= m_blockLast; ++line)
        rows.append(blockRow(line, m_blockColumn, lastColumn, QString(), false));
    setMode(Mode::Normal);
    if(command != "I" && command != "A") setRegister(rows.join('\n'), false, command == "y", true);
    if((command == "d" || command == "c") && !m_editor->isReadOnly())
        for(int line = m_blockLast; line >= m_blockFirst; --line)
            blockRow(line, m_blockColumn, lastColumn, QString(), true);
    if(command == "A") m_blockColumn = lastColumn;
    setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, m_blockFirst, m_blockColumn)));
    if((command == "I" || command == "A" || command == "c") && !m_editor->isReadOnly())
    {
        m_blockInsert = true; m_blockStart = currentPosition(); m_blockBefore = m_editor->text(); setMode(Mode::Insert);
    }
    else clampNormalCaret();
}

void VimInputHandler::finishBlockInsert()
{
    if(!m_blockInsert) return;
    flushInsertMappingPrefix(); m_blockInsert = false;
    const QByteArray before = m_blockBefore.toUtf8(), after = m_editor->text().toUtf8();
    int length = after.size()-before.size();
    // Broadcast only a simple insertion on the first row. Edits that leave this
    // range, or insert newlines, stay local instead of corrupting other rows.
    if(length <= 0 || after.left(m_blockStart) != before.left(m_blockStart) ||
       after.mid(m_blockStart+length) != before.mid(m_blockStart)) return;
    QString value = QString::fromUtf8(after.mid(m_blockStart, length));
    if(value.contains('\n') || value.contains('\r')) return;
    int caret = currentPosition();
    for(int line = m_blockLast; line > m_blockFirst; --line)
    {
        int position = int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, m_blockColumn));
        int actual = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, position));
        setPosition(position); m_editor->replaceSelectedText(QString(std::max(0, m_blockColumn-actual), ' ') + value);
    }
    setPosition(caret);
}
