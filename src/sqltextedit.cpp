#include "sql/ObjectIdentifier.h"
#include "sqltextedit.h"
#include "Settings.h"
#include "SqlUiLexer.h"
#include "VimInputHandler.h"
#include "NvimInputHandler.h"
#include <QAction>
#include <algorithm>

#include <Qsci/qscicommandset.h>
#include <Qsci/qscicommand.h>

#include <QShortcut>
#include <QRegularExpression>
#include <QLabel>
#include <QResizeEvent>

SqlUiLexer* SqlTextEdit::sqlLexer = nullptr;

SqlTextEdit::SqlTextEdit(QWidget* parent) :
    ExtendedScintilla(parent),
    m_vimInputHandler(new VimInputHandler(this, this)),
    m_vimModeIndicator(new QLabel(this)),
    m_nvimInputHandler(new NvimInputHandler(this, this))
{
    // Create lexer object if not done yet
    if(sqlLexer == nullptr)
        sqlLexer = new SqlUiLexer(this);

    // Set the SQL lexer
    setLexer(sqlLexer);

    // Set icons for auto completion
    registerImage(SqlUiLexer::ApiCompleterIconIdKeyword, QImage(":/icons/keyword"));
    registerImage(SqlUiLexer::ApiCompleterIconIdFunction, QImage(":/icons/function"));
    registerImage(SqlUiLexer::ApiCompleterIconIdTable, QImage(":/icons/table"));
    registerImage(SqlUiLexer::ApiCompleterIconIdColumn, QImage(":/icons/field"));
    registerImage(SqlUiLexer::ApiCompleterIconIdSchema, QImage(":/icons/database"));

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#define CAST_KEYS(k) QKeyCombination(k).toCombined()
#else
#define CAST_KEYS(k) k
#endif

    // Remove command bindings that would interfere with our shortcutToggleComment
    QsciCommand * command = standardCommands()->boundTo(CAST_KEYS(Qt::ControlModifier | Qt::Key_Slash));
    command->setKey(0);
    command = standardCommands()->boundTo(CAST_KEYS(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Slash));
    command->setKey(0);

    // Change command binding for Ctrl+T so it doesn't interfere with "Open tab"
    command = standardCommands()->boundTo(CAST_KEYS(Qt::ControlModifier | Qt::Key_T));
    command->setKey(CAST_KEYS(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Up));

    // Change command binding for Ctrl+Shift+T so it doesn't interfere with "Open SQL file"
    command = standardCommands()->boundTo(CAST_KEYS(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T));
    command->setKey(CAST_KEYS(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Insert));

#undef CAST_KEYS

    QShortcut* shortcutToggleComment = new QShortcut(QKeySequence(tr("Ctrl+/")), this, nullptr, nullptr, Qt::WidgetShortcut);
    connect(shortcutToggleComment, &QShortcut::activated, this, &SqlTextEdit::toggleBlockComment);

    m_vimModeIndicator->setAutoFillBackground(true);
    m_vimModeIndicator->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    m_vimModeIndicator->setMargin(3);
    m_vimModeIndicator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_vimModeIndicator->hide();
    connect(m_vimInputHandler, &VimInputHandler::modeChanged, this, &SqlTextEdit::updateVimModeIndicator);

    connect(m_nvimInputHandler, &NvimInputHandler::statusChanged, this, &SqlTextEdit::updateVimModeIndicator);
    connect(m_nvimInputHandler, &NvimInputHandler::saveRequested, this, [this]() {
        if(QAction* save = window()->findChild<QAction*>(QStringLiteral("actionSqlSaveFile"))) save->trigger();
    });
    connect(m_nvimInputHandler, &NvimInputHandler::executeRequested, this, [this]() {
        if(QAction* execute = window()->findChild<QAction*>(QStringLiteral("actionExecuteSql"))) execute->trigger();
    });
    connect(m_nvimInputHandler, &NvimInputHandler::failed, this, [this](const QString& message) {
        m_vimModeIndicator->setToolTip(message);
        // Text remains in the SQL editor if the native process is unavailable.
        m_vimInputHandler->setEnabled(Settings::getValue("editor", "vim_mode").toBool());
        updateVimModeIndicator();
    });

    // Do rest of initialisation
    reloadSettings();
}

void SqlTextEdit::reloadSettings()
{
    // Enable auto completion if it hasn't been disabled
    if(Settings::getValue("editor", "auto_completion").toBool())
    {
        setAutoCompletionThreshold(3);
        setAutoCompletionCaseSensitivity(true);
        setAutoCompletionShowSingle(true);
        setAutoCompletionSource(QsciScintilla::AcsAPIs);
    } else {
        setAutoCompletionThreshold(0);
    }
    // Set wrap lines
    setWrapMode(static_cast<QsciScintilla::WrapMode>(Settings::getValue("editor", "wrap_lines").toInt()));

    const bool vim = Settings::getValue("editor", "vim_mode").toBool();
    const bool native = vim && Settings::getValue("editor", "vim_native").toBool();
    if(!native) m_nvimInputHandler->stop();
    const bool started = native && m_nvimInputHandler->start();
    m_vimInputHandler->setEnabled(vim && !started);
    if(started) setAutoCompletionThreshold(0);
    updateVimModeIndicator();

    ExtendedScintilla::reloadSettings();

    setupSyntaxHighlightingFormat(sqlLexer, "comment", QsciLexerSQL::Comment);
    setupSyntaxHighlightingFormat(sqlLexer, "comment", QsciLexerSQL::CommentLine);
    setupSyntaxHighlightingFormat(sqlLexer, "comment", QsciLexerSQL::CommentDoc);
    setupSyntaxHighlightingFormat(sqlLexer, "keyword", QsciLexerSQL::Keyword);
    setupSyntaxHighlightingFormat(sqlLexer, "table", QsciLexerSQL::KeywordSet6);
    setupSyntaxHighlightingFormat(sqlLexer, "function", QsciLexerSQL::KeywordSet7);
    setupSyntaxHighlightingFormat(sqlLexer, "string", QsciLexerSQL::SingleQuotedString);

    // Highlight double quote strings as identifier or as literal string depending on user preference
    switch(static_cast<sqlb::escapeQuoting>(Settings::getValue("editor", "identifier_quotes").toInt())) {
    case sqlb::DoubleQuotes:
        setupSyntaxHighlightingFormat(sqlLexer, "identifier", QsciLexerSQL::DoubleQuotedString);
        sqlLexer->setQuotedIdentifiers(false);
        break;
    case sqlb::GraveAccents:
        sqlLexer->setQuotedIdentifiers(true);
        setupSyntaxHighlightingFormat(sqlLexer, "string", QsciLexerSQL::DoubleQuotedString);    // treat quoted string as literal string
        break;
    case sqlb::SquareBrackets:
        setupSyntaxHighlightingFormat(sqlLexer, "string", QsciLexerSQL::DoubleQuotedString);
        break;
    }
    setupSyntaxHighlightingFormat(sqlLexer, "identifier", QsciLexerSQL::Identifier);
    setupSyntaxHighlightingFormat(sqlLexer, "identifier", QsciLexerSQL::QuotedIdentifier);
}

void SqlTextEdit::resizeEvent(QResizeEvent* event)
{
    ExtendedScintilla::resizeEvent(event);
    if(m_vimModeIndicator->isVisible())
        m_vimModeIndicator->move(width() - m_vimModeIndicator->width() - 8,
                                 height() - m_vimModeIndicator->height() - 8);
}

void SqlTextEdit::updateVimModeIndicator()
{
    if(m_nvimInputHandler->isActive())
    {
        m_vimModeIndicator->setText(m_nvimInputHandler->status());
        m_vimModeIndicator->setToolTip(tr("Neovim configuration: %1").arg(NvimInputHandler::configDirectory()));
        m_vimModeIndicator->setMaximumWidth(std::max(100, width()-16));
        m_vimModeIndicator->adjustSize();
        m_vimModeIndicator->move(8, height()-m_vimModeIndicator->height()-8);
        m_vimModeIndicator->show(); m_vimModeIndicator->raise();
        return;
    }
    if(!m_vimInputHandler->isEnabled())
    {
        m_vimModeIndicator->hide();
        return;
    }

    switch(m_vimInputHandler->mode())
    {
    case VimInputHandler::Mode::Normal:
        m_vimModeIndicator->setText(tr("-- NORMAL --"));
        break;
    case VimInputHandler::Mode::Insert:
        m_vimModeIndicator->setText(tr("-- INSERT --"));
        break;
    case VimInputHandler::Mode::Visual:
        m_vimModeIndicator->setText(tr("-- VISUAL --"));
        break;
    case VimInputHandler::Mode::VisualLine:
        m_vimModeIndicator->setText(tr("-- VISUAL LINE --"));
        break;
    }

    m_vimModeIndicator->adjustSize();
    m_vimModeIndicator->move(width() - m_vimModeIndicator->width() - 8,
                             height() - m_vimModeIndicator->height() - 8);
    m_vimModeIndicator->show();
    m_vimModeIndicator->raise();
}


void SqlTextEdit::toggleBlockComment()
{
    int lineFrom, indexFrom, lineTo, indexTo;

    // If there is no selection, select the current line
    if (!hasSelectedText()) {
        getCursorPosition(&lineFrom, &indexFrom);

        indexTo = text(lineFrom).length();

        // Windows lines requires an adjustment, otherwise the selection would
        // end in the next line.
        if (text(lineFrom).endsWith("\r\n"))
            indexTo--;

        setSelection(lineFrom, 0, lineFrom, indexTo);
    }

    getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);

    bool uncomment = text(lineFrom).contains(QRegularExpression("^[ \t]*--"));

    // If the selection ends before the first character of a line, don't
    // take this line into account for un/commenting.
    if (indexTo==0)
        lineTo--;

    beginUndoAction();

    // Iterate over the selected lines, get line text, make
    // replacement depending on whether the first line was commented
    // or uncommented, and replace the line text. All in a single undo action.
    for (int line=lineFrom; line<=lineTo; line++) {
        QString lineText = text(line);

        if (uncomment)
            lineText.replace(QRegularExpression("^([ \t]*)-- ?"), "\\1");
        else
            lineText.replace(QRegularExpression("^"), "-- ");

        indexTo = text(line).length();
        if (lineText.endsWith("\r\n"))
            indexTo--;

        setSelection(line, 0, line, indexTo);
        replaceSelectedText(lineText);
    }
    endUndoAction();
}
