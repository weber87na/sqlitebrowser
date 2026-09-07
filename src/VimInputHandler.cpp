#include "VimInputHandler.h"

#include <Qsci/qsciscintilla.h>

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QInputDialog>
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
    m_editor->installEventFilter(this);
    m_mappingTimer->setSingleShot(true);
    m_mappingTimer->setInterval(700);
    connect(m_mappingTimer, &QTimer::timeout, this, &VimInputHandler::flushInsertMappingPrefix);
    QString config = qEnvironmentVariable("DB4S_VIM_CONFIG");
    if(config.isEmpty()) config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/vim.json";
    loadConfig(config);
    m_trackedText = m_editor->text().toUtf8();
    connect(m_editor, &QsciScintilla::textChanged, this, [this]() {
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
        if(m_groupOpen) m_editor->endUndoAction();
        m_editor->removeEventFilter(this);
    }
}

void VimInputHandler::setEnabled(bool enabled)
{
    if(m_enabled == enabled)
        return;

    if(m_groupOpen) { m_editor->endUndoAction(); m_groupOpen = false; }
    m_sequence.clear();
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
        if(m_mode == Mode::Insert)
            m_mappingTimer->start();
        return true;
    }

    if(m_mappingPrefix.isEmpty())
        return false;

    if(m_mode == Mode::Insert)
        flushInsertMappingPrefix();
    else
        m_mappingPrefix.clear();

    // The key which failed to complete a mapping may itself start one.
    if(isCustomMappingPrefix(key))
    {
        m_mappingPrefix = key;
        if(m_mode == Mode::Insert)
            m_mappingTimer->start();
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
                                 << "zh" << "zl" << "z;" << "z," << "zz" << "zt" << "zb";
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
        else { m_visualAnchor = currentPosition(); m_visualCaret = currentPosition(); setMode(Mode::VisualBlock); updateVisualSelection(); }
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
       (m_pendingCommand == "d" || m_pendingCommand == "c" || m_pendingCommand == "y"))
    { m_pendingCommand += key; return true; }
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

    if(m_pendingCommand == "g")
    {
        if(key == "g")
        {
            const int line = std::max(0, std::min(m_editor->lines() - 1, m_pendingCount - 1));
            m_editor->setCursorPosition(line, 0);
            clampNormalCaret();
        }
        resetPendingCommand();
        return true;
    }

    if(m_pendingCommand.length() == 2 && m_pendingCommand.endsWith("g"))
    {
        if(key == "g")
        {
            const int originalLine = currentLine();
            applyLineOperator(0, originalLine);
        }
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
    if(m_pendingCommand == "vi" || m_pendingCommand == "va")
    {
        setPosition(m_visualCaret);
        applyTextObject(key, m_pendingCommand == "va", takeCount());
        resetPendingCommand();
        return true;
    }
    if(m_pendingCommand == "\"") return extendedPending(key);
    if(m_pendingCommand.isEmpty() && key == "\"")
    { m_pendingCommand = key; m_pendingCount = takeCount(); return true; }
    if(key == ":") { promptCommand(); return true; }
    if(m_mode == Mode::VisualBlock && (key == "d" || key == "x" || key == "y" || key == "c" || key == "I" || key == "A"))
    { finishBlockOperator(key == "x" ? "d" : key); return true; }
    if((key == "p" || key == "P") && m_mode != Mode::VisualBlock)
    {
        if(!m_editor->isReadOnly())
        {
            const int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
            const QString value = QApplication::clipboard()->text();
            m_editor->replaceSelectedText(value); setPosition(first);
        }
        setMode(Mode::Normal); return true;
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
        for(int i = 0; i < count; ++i)
        {
            const int c = category(position);
            while(position > 0 && category(positionBefore(position)) == c)
                position = positionBefore(position);
            if(position > 0) position = positionBefore(position);
            while(position > 0 && category(position) == 0) position = positionBefore(position);
        }
        setPosition(position);
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
    if((command == "%" || command == "ge" || command == "gE") && last < documentLength())
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
        const QStringList rows = value.split('\n');
        int line = currentLine(), column = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, currentPosition()));
        if(!before) ++column;
        for(int i = 0; i < rows.size(); ++i)
        {
            while(line+i >= m_editor->lines()) { setPosition(documentLength()); m_editor->replaceSelectedText(endOfLine()); }
            int pos = int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line+i, column));
            int actual = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, pos));
            setPosition(pos); m_editor->replaceSelectedText(QString(std::max(0, column-actual), ' ') + rows.at(i).repeated(count));
        }
        setPosition(int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, column))); return;
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

void VimInputHandler::enterVisualMode(bool linewise)
{
    resetPendingCommand();
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

void VimInputHandler::promptSearch(bool forward)
{
    bool accepted = false;
    const QString text = QInputDialog::getText(m_editor, tr("Vim search"),
                                                forward ? tr("Find forward:") : tr("Find backward:"),
                                                QLineEdit::Normal, m_lastSearch, &accepted);
    if(!accepted || text.isEmpty())
        return;

    m_lastSearch = text;
    m_lastSearchForward = forward;
    repeatSearch(false);
}

void VimInputHandler::repeatSearch(bool reverse)
{
    if(m_lastSearch.isEmpty())
        return;

    const bool forward = reverse ? !m_lastSearchForward : m_lastSearchForward;
    int line = 0;
    int index = 0;
    m_editor->getCursorPosition(&line, &index);

    // Move one character first so repeated searches do not find the same item.
    const int start = currentPosition();
    if(forward && start < documentLength())
        setPosition(positionAfter(start));
    else if(!forward && start > 0)
        setPosition(positionBefore(start));
    m_editor->getCursorPosition(&line, &index);

    if(m_editor->findFirst(m_lastSearch, true, true, false, true, forward,
                           line, index, true, true, true))
    {
        int lineFrom = 0;
        int indexFrom = 0;
        int lineTo = 0;
        int indexTo = 0;
        m_editor->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
        m_editor->setCursorPosition(lineFrom, indexFrom);
        clampNormalCaret();
    }
    else
    {
        setPosition(start);
    }
}

bool VimInputHandler::processStroke(QKeyEvent* event)
{
    const QString key = commandKey(event);
    if(m_mode == Mode::Normal && m_pendingCommand.isEmpty() && key == "q" && !m_recording.isEmpty())
    { m_recording.clear(); return true; }
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
    if(m_mode == Mode::Normal && m_pendingCommand.isEmpty() && m_mappingPrefix.isEmpty() && m_count == 0 && m_selectedRegister.isEmpty())
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
    return true;
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
        QString command = m_findCommand;
        if(key == "," && !command.isEmpty()) command = command == command.toUpper() ? command.toLower() : command.toUpper();
        int original = currentPosition();
        if(command.toLower() == "t")
            setPosition(command == command.toLower() ? positionAfter(original) : positionBefore(original));
        if(!findCharacter(command, m_findTarget, takeCount())) setPosition(original);
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
        int start = currentPosition();
        bool found = findCharacter(command, key, m_pendingCount * takeCount());
        m_findCommand = command; m_findTarget = key;
        if(found && pending.size() > 1)
        {
            int target = currentPosition(); m_pendingCommand = pending.left(pending.size()-1);
            if(target >= start) applyCharacterOperator(start, positionAfter(target));
            else applyCharacterOperator(target, start);
        }
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
    if(pending.endsWith("g") && (key == "e" || key == "E"))
    {
        const int count = m_pendingCount * takeCount();
        if(pending == "g") move("g" + key, count);
        else
        {
            m_pendingCommand.chop(1);
            applyOperatorMotion("g" + key, count);
        }
        resetPendingCommand(); return true;
    }
    if(pending == "g" && key == "_")
    {
        int p = lineEndPosition(currentLine());
        while(p > positionFromLine(currentLine()) && characterClassAt(positionBefore(p)) == 0) p = positionBefore(p);
        setPosition(p > positionFromLine(currentLine()) ? positionBefore(p) : p); resetPendingCommand(); return true;
    }
    if(pending == "g" && key == "J") { int count = m_pendingCount; resetPendingCommand(); joinLines(count, true); return true; }
    if(pending == "g" && key == "v")
    {
        resetPendingCommand(); setMode(m_savedVisualMode);
        m_visualAnchor = std::min(m_savedAnchor, documentLength()); m_visualCaret = std::min(m_savedCaret, documentLength());
        updateVisualSelection(); return true;
    }
    if((pending == "gu" || pending == "gU" || pending == "g~") && (key == pending.right(1) || key == "g"))
    { applyLineOperator(currentLine(), currentLine()+m_pendingCount-1); resetPendingCommand(); return true; }
    return false;
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
    setPosition(position); return true;
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
        m_commandLine->setPlaceholderText(QString::fromUtf8("輸入指令，例如 %s/foo/bar/g；Esc 取消"));
        connect(m_commandLine, &QLineEdit::returnPressed, this, [this]() {
            const QString command = m_commandLine->text();
            if(executeCommand(command)) { m_commandLine->hide(); m_editor->setFocus(); }
            else m_commandLine->setToolTip(QString::fromUtf8("指令不支援、格式錯誤或找不到符合項目"));
        });
        auto* cancel = new QAction(m_commandLine);
        cancel->setShortcut(QKeySequence(Qt::Key_Escape)); cancel->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_commandLine->addAction(cancel);
        connect(cancel, &QAction::triggered, this, [this]() { m_commandLine->hide(); m_editor->setFocus(); });
    }
    m_commandLine->setGeometry(4, m_editor->height()-32, std::max(60, m_editor->width()-8), 28);
    m_commandLine->setText(visualRange ? "'<,'>" : QString());
    m_commandLine->show(); m_commandLine->raise(); m_commandLine->setFocus();
}

bool VimInputHandler::executeCommand(const QString& input)
{
    QString command = input.trimmed();
    if(command.startsWith(':')) command = command.mid(1).trimmed();
    if(command == "w" || command == "write")
    {
        if(auto* action = m_editor->window()->findChild<QAction*>("actionSqlSaveFile")) { action->trigger(); return true; }
        return false;
    }
    if(command == "noh" || command == "nohlsearch") { setPosition(currentPosition()); return true; }

    // Scintilla exposes an extra empty line after a final EOL. Ex addresses,
    // unlike editor cursor positions, refer to actual buffer lines only.
    int lineCount = m_editor->lines();
    if(lineCount > 1 && positionFromLine(lineCount - 1) == documentLength()) --lineCount;
    const int cursorLine = std::min(currentLine(), lineCount - 1);
    int firstLine = cursorLine, lastLine = cursorLine;
    bool hasRange = false;
    int offset = 0;
    const auto skipSpace = [&]() {
        while(offset < command.size() && command.at(offset).isSpace()) ++offset;
    };
    const auto parseAddress = [&](int relativeLine, int& result) {
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
            if(!valid || value < 1 || value > std::numeric_limits<int>::max()) return false;
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
        if(value < 0 || value >= lineCount) return false;
        result = int(value);
        return true;
    };
    if(command.startsWith('%'))
    {
        firstLine = 0; lastLine = lineCount - 1; hasRange = true; ++offset;
    }
    else if(!command.isEmpty() && (command.at(0).isDigit() || QString(".$'+-").contains(command.at(0))))
    {
        hasRange = true;
        if(!parseAddress(cursorLine, firstLine)) return false;
        lastLine = firstLine;
        if(offset < command.size() && (command.at(offset) == ',' || command.at(offset) == ';'))
        {
            const bool relativeToFirst = command.at(offset++) == ';';
            if(!parseAddress(relativeToFirst ? firstLine : cursorLine, lastLine)) return false;
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
    if(!command.startsWith('s') || command.size() < 2) return false;
    const QChar delimiter = command.at(1);
    if(delimiter.isLetterOrNumber() || delimiter.isSpace() || delimiter == '\\' || delimiter == '|') return false;
    QStringList parts; QString part;
    for(int i = 2; i < command.size(); ++i)
    {
        if(command.at(i) == '\\')
        {
            if(i+1 == command.size()) return false;
            if(command.at(i+1) == delimiter) part += delimiter;
            else { part += command.at(i); part += command.at(i+1); }
            ++i;
        }
        else if(command.at(i) == delimiter) { parts << part; part.clear(); }
        else part += command.at(i);
    }
    parts << part; if(parts.size() < 2 || parts.size() > 3) return false;
    QString flags = parts.size() == 3 ? parts.at(2) : QString();
    if(flags.contains(QRegularExpression("[^gi]"))) return false;
    QRegularExpression expression(parts.at(0), flags.contains('i') ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    if(!expression.isValid() || m_editor->isReadOnly()) return false;
    bool changed = false; m_editor->beginUndoAction();
    for(int n = lastLine; n >= firstLine; --n)
    {
        int first = positionFromLine(n), last = lineEndPosition(n);
        QString value = QString::fromUtf8(m_editor->text().toUtf8().mid(first, last-first));
        auto matches = expression.globalMatch(value); QVector<QRegularExpressionMatch> found;
        while(matches.hasNext()) { found << matches.next(); if(!flags.contains('g')) break; }
        for(int i = found.size()-1; i >= 0; --i)
        {
            const auto match = found.at(i); QString replacement = parts.at(1);
            for(int group = std::min(9, match.lastCapturedIndex()); group >= 1; --group) replacement.replace("\\" + QString::number(group), match.captured(group));
            replacement.replace("&", match.captured());
            value.replace(match.capturedStart(), match.capturedLength(), replacement); changed = true;
        }
        if(!found.isEmpty()) { setSelection(first, last); m_editor->replaceSelectedText(value); }
    }
    m_editor->endUndoAction(); setPosition(positionFromLine(firstLine)); clampNormalCaret(); return changed;
}

void VimInputHandler::finishBlockOperator(const QString& command)
{
    const int anchorLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualAnchor));
    const int caretLine = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, m_visualCaret));
    const int anchorColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualAnchor));
    const int caretColumn = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN, m_visualCaret));
    m_blockFirst = std::min(anchorLine, caretLine); m_blockLast = std::max(anchorLine, caretLine);
    m_blockColumn = std::min(anchorColumn, caretColumn);
    const int lastColumn = std::max(anchorColumn, caretColumn)+1;
    QStringList rows;
    QVector<QPair<int,int>> ranges;
    const QByteArray bytes = m_editor->text().toUtf8();
    for(int line = m_blockFirst; line <= m_blockLast; ++line)
    {
        int first = int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, m_blockColumn));
        int last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, line, lastColumn));
        ranges.append(qMakePair(first, last)); rows.append(QString::fromUtf8(bytes.mid(first, last-first)));
    }
    setMode(Mode::Normal);
    if(command != "I" && command != "A") setRegister(rows.join('\n'), false, command == "y", true);
    if((command == "d" || command == "c") && !m_editor->isReadOnly())
        for(int i = ranges.size()-1; i >= 0; --i)
        { setSelection(ranges.at(i).first, ranges.at(i).second); m_editor->replaceSelectedText(QString()); }
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
