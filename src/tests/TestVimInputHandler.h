#ifndef TESTVIMINPUTHANDLER_H
#define TESTVIMINPUTHANDLER_H

#include <QObject>

class TestVimInputHandler : public QObject
{
    Q_OBJECT

private slots:
    void blockEditing();
    void configMappings();
    void findRepeatAndMarks();
    void builtinCommands_data();
    void builtinCommands();
    void repeatAndMacro();
    void substitution();
    void surround_data();
    void surround();
    void textObjects_data();
    void textObjects();
    void enhancedMotions();
    void mappingPrefixAndEscape();
    void insertAndEscape();
    void insertCtrlWDeletesPreviousWord();
    void normalMotionsAndDelete();
    void wordEndMotionIncludesPunctuationAcrossLines();
    void customInsertMappings();
    void customNormalMappings();
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
