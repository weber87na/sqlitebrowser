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

#include <algorithm>
#include <cctype>

namespace
{
constexpr int MaximumCount = 9999;

QString commandKey(const QKeyEvent* event)
{
    // QKeyEvent::text() can remain lower-case for synthetic events and on some
    // keyboard layouts.  Letter commands must still distinguish v from V, etc.
    if(event->modifiers().testFlag(Qt::ShiftModifier) &&
       event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
    {
        return QString(QChar::fromLatin1(static_cast<char>(event->key())));
    }

    return event->text();
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
}

VimInputHandler::~VimInputHandler()
{
    if(m_editor)
        m_editor->removeEventFilter(this);
}

void VimInputHandler::setEnabled(bool enabled)
{
    if(m_enabled == enabled)
        return;

    m_enabled = enabled;
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
    if(!m_enabled || watched != m_editor)
        return QObject::eventFilter(watched, event);

    if(event->type() == QEvent::FocusOut || event->type() == QEvent::InputMethod)
        flushInsertMappingPrefix();
    if(event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);

    auto* key = static_cast<QKeyEvent*>(event);
    if(m_mode == Mode::Insert && (key->text().isEmpty() ||
       key->modifiers().testFlag(Qt::ControlModifier)))
        flushInsertMappingPrefix();

    return handleKeyPress(static_cast<QKeyEvent*>(event));
}

bool VimInputHandler::handleKeyPress(QKeyEvent* event)
{
    const bool escape = event->key() == Qt::Key_Escape ||
        (event->key() == Qt::Key_BracketLeft && event->modifiers().testFlag(Qt::ControlModifier));

    if(escape)
    {
        if(m_mode == Mode::Insert)
            flushInsertMappingPrefix();
        else
            m_mappingPrefix.clear();

        if(m_mode == Mode::Insert)
        {
            if(currentPosition() > positionFromLine(currentLine()))
                setPosition(positionBefore(currentPosition()));
            setMode(Mode::Normal);
            clampNormalCaret();
        }
        else
        {
            setPosition(m_mode == Mode::Visual || m_mode == Mode::VisualLine ? m_visualCaret : currentPosition());
            setMode(Mode::Normal);
        }
        resetPendingCommand();
        return true;
    }

    if(m_mode == Mode::Insert && event->key() == Qt::Key_W &&
        event->modifiers().testFlag(Qt::ControlModifier))
    {
        m_editor->SendScintilla(QsciScintillaBase::SCI_DELWORDLEFT);
        return true;
    }

    if(handleCustomMapping(event))
        return true;

    if(m_mode == Mode::Insert)
        return false;

    if(event->modifiers().testFlag(Qt::AltModifier) || event->modifiers().testFlag(Qt::MetaModifier))
        return false;

    if(event->modifiers().testFlag(Qt::ControlModifier))
        return handleControlKey(event);

    if(m_mode == Mode::Visual || m_mode == Mode::VisualLine)
        return handleVisualKey(event);

    return handleNormalKey(event);
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

    const QString key = commandKey(event);
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
        mappings = QStringList() << ",," << ",ss" << ",ci" << ",xs"
                                 << "zh" << "zl" << "z;" << "z,";
    else
        mappings = QStringList() << ",," << ",aa" << ",ci" << ",ss";

    for(const QString& candidate : mappings)
    {
        if(candidate.startsWith(mapping) && candidate != mapping)
            return true;
    }
    return false;
}

bool VimInputHandler::executeCustomMapping(const QString& mapping)
{
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
        if(m_mode == Mode::Visual || m_mode == Mode::VisualLine)
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
    if(m_mode == Mode::Insert && !m_mappingPrefix.isEmpty() && !m_editor->isReadOnly())
        m_editor->replaceSelectedText(m_mappingPrefix);
    m_mappingPrefix.clear();
}

bool VimInputHandler::handleControlKey(QKeyEvent* event)
{
    const int count = (event->key() == Qt::Key_D || event->key() == Qt::Key_U)
        ? takeCount() : 1;
    switch(event->key())
    {
    case Qt::Key_R:
        m_editor->redo();
        return true;
    case Qt::Key_D:
        for(int i = 0; i < count; ++i)
            m_editor->SendScintilla(QsciScintillaBase::SCI_PAGEDOWN);
        clampNormalCaret();
        return true;
    case Qt::Key_U:
        for(int i = 0; i < count; ++i)
            m_editor->SendScintilla(QsciScintillaBase::SCI_PAGEUP);
        clampNormalCaret();
        return true;
    default:
        // Keep application shortcuts such as Ctrl+S, Ctrl+F and Ctrl+Enter.
        return false;
    }
}

bool VimInputHandler::handleNormalKey(QKeyEvent* event)
{
    const QString key = commandKey(event);

    if(key.length() == 1 && key.at(0).isDigit() && !(key == "0" && m_count == 0))
    {
        m_count = std::min(MaximumCount, m_count * 10 + key.toInt());
        return true;
    }

    if(!m_pendingCommand.isEmpty())
        return handlePendingKey(event);

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
    if(m_mode == mode)
    {
        emit modeChanged();
        return;
    }

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
        clampNormalCaret();
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

bool VimInputHandler::applyOperatorMotion(const QString& command, int count)
{
    const int start = currentPosition();
    const int startLine = currentLine();

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

    if(!move(command, count))
    {
        setPosition(start);
        return false;
    }

    int target = command == "$" ? lineEndPosition(currentLine()) : currentPosition();
    if((command == "e" || command == "E") && target >= start)
        target = positionAfter(target);
    int first = std::min(start, target);
    int last = std::max(start, target);
    if(command == "%" && last < documentLength())
        last = positionAfter(last);
    applyCharacterOperator(first, last);
    return true;
}

void VimInputHandler::applyCharacterOperator(int start, int end)
{
    if(end <= start)
    {
        setPosition(start);
        return;
    }

    setSelection(start, end);
    setRegister(m_editor->selectedText(), false);

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

    setRegister(linesText(firstLine, lastLine), true);
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

void VimInputHandler::joinLines(int count)
{
    if(m_editor->isReadOnly() || currentLine() >= m_editor->lines() - 1)
        return;

    const int start = currentPosition();
    const int firstLine = currentLine();
    const int lastLine = std::min(m_editor->lines() - 1, firstLine + std::max(2, count) - 1);
    setSelection(positionFromLine(firstLine), lineEndPosition(lastLine));
    m_editor->SendScintilla(QsciScintillaBase::SCI_LINESJOIN);
    setPosition(start);
    clampNormalCaret();
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

void VimInputHandler::setRegister(const QString& text, bool linewise)
{
    m_registerText = text;
    m_registerLinewise = linewise;
    QApplication::clipboard()->setText(text);
}

void VimInputHandler::paste(bool before, int count)
{
    if(m_editor->isReadOnly())
        return;

    QString value = QApplication::clipboard()->text();
    bool linewise = !m_registerText.isEmpty() && value == m_registerText && m_registerLinewise;
    if(value.isEmpty())
    {
        value = m_registerText;
        linewise = m_registerLinewise;
    }
    if(value.isEmpty())
        return;

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

    setRegister(m_editor->selectedText(), false);

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

    if(m_editor->findFirst(m_lastSearch, false, true, false, true, forward,
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
