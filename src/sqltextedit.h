#ifndef SQLTEXTEDIT_H
#define SQLTEXTEDIT_H

#include "ExtendedScintilla.h"

class SqlUiLexer;
class VimInputHandler;
class NvimInputHandler;
class QLabel;
class QResizeEvent;

/**
 * @brief The SqlTextEdit class
 * This class is based on the QScintilla widget
 */
class SqlTextEdit : public ExtendedScintilla
{
    Q_OBJECT

public:
    explicit SqlTextEdit(QWidget *parent = nullptr);

    static SqlUiLexer* sqlLexer;

public slots:
    void reloadSettings();
    void toggleBlockComment();

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateVimModeIndicator();

private:
    VimInputHandler* m_vimInputHandler;
    QLabel* m_vimModeIndicator;
    NvimInputHandler* m_nvimInputHandler;

};

#endif
