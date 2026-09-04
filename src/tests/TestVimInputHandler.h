#ifndef TESTVIMINPUTHANDLER_H
#define TESTVIMINPUTHANDLER_H

#include <QObject>

class TestVimInputHandler : public QObject
{
    Q_OBJECT

private slots:
    void insertAndEscape();
    void normalMotionsAndDelete();
    void countedLineDelete();
    void visualDelete();
    void yankAndPasteLine();
    void undoAndRedo();
    void operatorMotion();
    void matchingBraceOperator();
    void appendAndOpenLines();
    void visualLineDelete();
    void windowsLineEndings();
    void externalClipboardPaste();
    void disabledLeavesEditorUnchanged();
};

#endif
