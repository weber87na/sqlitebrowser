#ifndef TESTVIMINPUTHANDLER_H
#define TESTVIMINPUTHANDLER_H

#include <QObject>

class TestVimInputHandler : public QObject
{
    Q_OBJECT

private slots:
    void changeHistory();
    void registerHistories_data();
    void registerHistories();
    void numberedRegisterRotation();
    void registerPasteShapes();
    void blockRegisterHistory();
    void insertRegisterClipboardAndCancel();
    void insertRegisterCountRepeatUndo();
    void readOnlyRegisterOperations();
    void lineStartMotions();
    void backwardEndOperators();
    void insertControlDeletion();
    void insertCompletionCycles();
    void insertCompletionUnicodeAndSafety();
    void insertCompletionResets();
    void insertCompletionDotRepeat();
    void insertCompletionRepeatEdges();
    void insertCompletionMacroIsDynamic();
    void insertBackspaceAnchors();
    void replaceBackspacing();
    void countedInsert();
    void numericAndSqlMappings();
    void blockEditing();
    void configMappings();
    void findRepeatAndMarks();
    void builtinCommands_data();
    void builtinCommands();
    void repeatAndMacro();
    void substitution();
    void exLineCommands_data();
    void exLineCommands();
    void exRangesAndMarks();
    void exRejectsInvalidCommands();
    void exVisualRange();
    void surround_data();
    void surround();
    void paragraphAndSentenceObjects_data();
    void paragraphAndSentenceObjects();
    void paragraphAndSentenceSelections();
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
