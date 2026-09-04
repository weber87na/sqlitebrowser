#include "TestVimInputHandler.h"

#include "VimInputHandler.h"

#include <Qsci/qsciscintilla.h>

#include <QApplication>
#include <QClipboard>
#include <QtTest/QTest>

namespace
{
void prepareEditor(QsciScintilla& editor)
{
    editor.resize(640, 320);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    QCoreApplication::processEvents();
}
}

void TestVimInputHandler::insertAndEscape()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_I);
    QTest::keyClicks(&editor, "select 1");
    QTest::keyClick(&editor, Qt::Key_Escape);

    QCOMPARE(editor.text(), QString("select 1"));
    QVERIFY(handler.mode() == VimInputHandler::Mode::Normal);
}

void TestVimInputHandler::normalMotionsAndDelete()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("one two");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_W);
    QTest::keyClick(&editor, Qt::Key_X);

    QCOMPARE(editor.text(), QString("one wo"));
}

void TestVimInputHandler::countedLineDelete()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("one\ntwo\nthree\nfour");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_2);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);

    QCOMPARE(editor.text(), QString("three\nfour"));
}

void TestVimInputHandler::visualDelete()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("abcd");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_V);
    QTest::keyClick(&editor, Qt::Key_L);
    QTest::keyClick(&editor, Qt::Key_D);

    QCOMPARE(editor.text(), QString("cd"));
    QVERIFY(handler.mode() == VimInputHandler::Mode::Normal);
}

void TestVimInputHandler::yankAndPasteLine()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("one\ntwo");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_Y);
    QTest::keyClick(&editor, Qt::Key_Y);
    QTest::keyClick(&editor, Qt::Key_J);
    QTest::keyClick(&editor, Qt::Key_P);

    QCOMPARE(editor.text(), QString("one\ntwo\none"));
}

void TestVimInputHandler::undoAndRedo()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("abc");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_X);
    QCOMPARE(editor.text(), QString("bc"));
    QTest::keyClick(&editor, Qt::Key_U);
    QCOMPARE(editor.text(), QString("abc"));
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("bc"));
}

void TestVimInputHandler::operatorMotion()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("one two three");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_W);

    QCOMPARE(editor.text(), QString("two three"));
}

void TestVimInputHandler::matchingBraceOperator()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("(value)");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_Percent);

    QCOMPARE(editor.text(), QString());
}

void TestVimInputHandler::appendAndOpenLines()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("abc");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_A, Qt::ShiftModifier);
    QTest::keyClicks(&editor, "!");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClick(&editor, Qt::Key_O);
    QTest::keyClicks(&editor, "next");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClick(&editor, Qt::Key_O, Qt::ShiftModifier);
    QTest::keyClicks(&editor, "middle");
    QTest::keyClick(&editor, Qt::Key_Escape);

    QCOMPARE(editor.text(), QString("abc!\nmiddle\nnext"));
}

void TestVimInputHandler::visualLineDelete()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("one\ntwo\nthree");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_V, Qt::ShiftModifier);
    QTest::keyClick(&editor, Qt::Key_J);

    QVERIFY(handler.mode() == VimInputHandler::Mode::VisualLine);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART), 0L);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND), 8L);

    QTest::keyClick(&editor, Qt::Key_D);

    QCOMPARE(editor.text(), QString("three"));
}

void TestVimInputHandler::windowsLineEndings()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setEolMode(QsciScintilla::EolWindows);
    editor.setText("one\r\ntwo");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_Y);
    QTest::keyClick(&editor, Qt::Key_Y);
    QTest::keyClick(&editor, Qt::Key_J);
    QTest::keyClick(&editor, Qt::Key_P);

    QCOMPARE(editor.text(), QString("one\r\ntwo\r\none"));
}

void TestVimInputHandler::externalClipboardPaste()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("abc");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_Y);
    QTest::keyClick(&editor, Qt::Key_Y);
    QApplication::clipboard()->setText("X");
    QTest::keyClick(&editor, Qt::Key_P);

    QCOMPARE(editor.text(), QString("aXbc"));
}

void TestVimInputHandler::disabledLeavesEditorUnchanged()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);

    QTest::keyClicks(&editor, "vim");

    QCOMPARE(editor.text(), QString("vim"));
    QVERIFY(!handler.isEnabled());
}

QTEST_MAIN(TestVimInputHandler)
