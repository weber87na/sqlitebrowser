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

void TestVimInputHandler::insertCtrlWDeletesPreviousWord()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_I);
    QTest::keyClicks(&editor, "select customer");
    QTest::keyClick(&editor, Qt::Key_W, Qt::ControlModifier);

    QCOMPARE(editor.text(), QString("select "));
    QVERIFY(handler.mode() == VimInputHandler::Mode::Insert);
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

void TestVimInputHandler::wordEndMotionIncludesPunctuationAcrossLines()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("SELECT * \nFROM test;");
    prepareEditor(editor);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_E);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 5L);
    QTest::keyClick(&editor, Qt::Key_E);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 7L);
    QTest::keyClick(&editor, Qt::Key_E);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 13L);
}

void TestVimInputHandler::customInsertMappings()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("    SELECT");
    prepareEditor(editor);
    editor.setCursorPosition(0, 4);
    handler.setEnabled(true);

    QTest::keyClick(&editor, Qt::Key_I);
    QTest::keyClicks(&editor, "z;");
    QCOMPARE(editor.text(), QString("    SELECT;"));
    QVERIFY(handler.mode() == VimInputHandler::Mode::Insert);

    QTest::keyClicks(&editor, ",,");
    QVERIFY(handler.mode() == VimInputHandler::Mode::Normal);
}

void TestVimInputHandler::customNormalMappings()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("    SELECT");
    prepareEditor(editor);
    editor.setCursorPosition(0, 6);
    handler.setEnabled(true);

    QTest::keyClicks(&editor, "zh");
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 4L);
    QTest::keyClicks(&editor, "z,");
    QCOMPARE(editor.text(), QString("    SELECT,"));
    QVERIFY(handler.mode() == VimInputHandler::Mode::Normal);
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

void TestVimInputHandler::enhancedMotions()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("  abc.def ghi");
    prepareEditor(editor);
    editor.setCursorPosition(0, 2);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "^^");
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 2L);
    QTest::keyClicks(&editor, "w");
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 5L);
    QTest::keyClicks(&editor, "w");
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 6L);
    QTest::keyClick(&editor, Qt::Key_B, Qt::ShiftModifier);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 2L);
    QTest::keyClick(&editor, Qt::Key_E, Qt::ShiftModifier);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 8L);
    QTest::keyClick(&editor, Qt::Key_W, Qt::ShiftModifier);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 10L);
    QTest::keyClicks(&editor, "rz");
    QCOMPARE(editor.text(), QString("  abc.def zhi"));
}

void TestVimInputHandler::mappingPrefixAndEscape()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("abcd");
    prepareEditor(editor);
    editor.setCursorPosition(0, 2);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS), 1L);
    QTest::keyClicks(&editor, "iz");
    QTest::keyClick(&editor, Qt::Key_Right);
    QCOMPARE(editor.text(), QString("azbcd"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::qWait(750);
    QCOMPARE(editor.text(), QString("azbcd"));
}

void TestVimInputHandler::textObjects_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("column");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("insert");
    QTest::newRow("inner-word") << QStringLiteral("select customer from users") << 10 << QStringLiteral("diw") << QStringLiteral("select  from users") << false;
    QTest::newRow("around-word") << QStringLiteral("select customer from users") << 10 << QStringLiteral("daw") << QStringLiteral("select from users") << false;
    QTest::newRow("two-words") << QStringLiteral("select customer from users") << 10 << QStringLiteral("d2iw") << QStringLiteral("select  users") << false;
    QTest::newRow("WORD") << QStringLiteral("select schema.table from t") << 10 << QStringLiteral("ciW") << QStringLiteral("select  from t") << true;
    QTest::newRow("inner-quotes") << QStringLiteral("select 'customer'") << 10 << QStringLiteral("ci'") << QStringLiteral("select ''") << true;
    QTest::newRow("around-quotes") << QStringLiteral("select 'customer'") << 10 << QStringLiteral("da'") << QStringLiteral("select ") << false;
    QTest::newRow("nested") << QStringLiteral("select (a + (b * c))") << 13 << QStringLiteral("di(") << QStringLiteral("select (a + ())") << false;
    QTest::newRow("outer-nested") << QStringLiteral("select (a + (b * c))") << 13 << QStringLiteral("d2i(") << QStringLiteral("select ()") << false;
    QTest::newRow("closing-brace") << QStringLiteral("select (abc)") << 11 << QStringLiteral("da(") << QStringLiteral("select ") << false;
    QTest::newRow("empty-change") << QStringLiteral("select ()") << 8 << QStringLiteral("ci(") << QStringLiteral("select ()") << true;
    QTest::newRow("unmatched") << QStringLiteral("select (abc") << 9 << QStringLiteral("di(") << QStringLiteral("select (abc") << false;
    QTest::newRow("cw-separator") << QStringLiteral("one two") << 0 << QStringLiteral("cw") << QStringLiteral(" two") << true;
    QTest::newRow("cw-last-character") << QStringLiteral("one two") << 2 << QStringLiteral("cw") << QStringLiteral("on two") << true;
    QTest::newRow("counted-cw") << QStringLiteral("one two three") << 0 << QStringLiteral("c2w") << QStringLiteral(" three") << true;
    QTest::newRow("visual-word") << QStringLiteral("one two") << 1 << QStringLiteral("viwd") << QStringLiteral(" two") << false;
    QTest::newRow("visual-brackets") << QStringLiteral("select (abc)") << 9 << QStringLiteral("va(d") << QStringLiteral("select ") << false;
    QTest::newRow("empty-document") << QStringLiteral("") << 0 << QStringLiteral("di(") << QStringLiteral("") << false;
    QTest::newRow("unicode-quotes") << QStringLiteral("select '中文名稱'") << 9 << QStringLiteral("ci'") << QStringLiteral("select ''") << true;
    QTest::newRow("cancel") << QStringLiteral("one two") << 0 << QStringLiteral("di") << QStringLiteral("one two") << false;
}

void TestVimInputHandler::textObjects()
{
    QFETCH(QString, input);
    QFETCH(int, column);
    QFETCH(QString, keys);
    QFETCH(QString, expected);
    QFETCH(bool, insert);
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText(input);
    prepareEditor(editor);
    editor.setCursorPosition(0, column);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, keys);
    QCOMPARE(editor.text(), expected);
    QCOMPARE(handler.mode() == VimInputHandler::Mode::Insert, insert);
    QTest::keyClick(&editor, Qt::Key_Escape);
    if(input != expected)
    {
        QTest::keyClick(&editor, Qt::Key_U);
        QCOMPARE(editor.text(), input);
    }
}

void TestVimInputHandler::surround_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("column");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("readOnly");
    QTest::newRow("add-word") << QStringLiteral("one two") << 1 << QStringLiteral("ysiw\"") << QStringLiteral("\"one\" two") << false;
    QTest::newRow("add-padded") << QStringLiteral("one two") << 1 << QStringLiteral("ysiw(") << QStringLiteral("( one ) two") << false;
    QTest::newRow("add-tight") << QStringLiteral("one two") << 1 << QStringLiteral("ysiw)") << QStringLiteral("(one) two") << false;
    QTest::newRow("line") << QStringLiteral("  select *") << 3 << QStringLiteral("yss]") << QStringLiteral("  [select *]") << false;
    QTest::newRow("motion") << QStringLiteral("one two") << 0 << QStringLiteral("yse\"") << QStringLiteral("\"one\" two") << false;
    QTest::newRow("two-words") << QStringLiteral("one two three") << 0 << QStringLiteral("ys2iw\"") << QStringLiteral("\"one two\" three") << false;
    QTest::newRow("change") << QStringLiteral("\"one\" two") << 2 << QStringLiteral("cs\"' ") << QStringLiteral("'one' two") << false;
    QTest::newRow("delete") << QStringLiteral("\"one\" two") << 2 << QStringLiteral("ds\"") << QStringLiteral("one two") << false;
    QTest::newRow("padding") << QStringLiteral("( one )") << 3 << QStringLiteral("ds(") << QStringLiteral("one") << false;
    QTest::newRow("nested") << QStringLiteral("((one))") << 3 << QStringLiteral("2ds(") << QStringLiteral("(one)") << false;
    QTest::newRow("empty") << QStringLiteral("()") << 1 << QStringLiteral("cs)\"") << QStringLiteral("\"\"") << false;
    QTest::newRow("visual") << QStringLiteral("one two") << 1 << QStringLiteral("viwS\"") << QStringLiteral("\"one\" two") << false;
    QTest::newRow("unicode") << QStringLiteral("select '中文'") << 9 << QStringLiteral("cs'\"") << QStringLiteral("select \"中文\"") << false;
    QTest::newRow("missing") << QStringLiteral("one two") << 1 << QStringLiteral("ds\"") << QStringLiteral("one two") << false;
    QTest::newRow("invalid") << QStringLiteral("one two") << 1 << QStringLiteral("ysiwq") << QStringLiteral("one two") << false;
    QTest::newRow("cancel") << QStringLiteral("one two") << 1 << QStringLiteral("ysiw") << QStringLiteral("one two") << false;
    QTest::newRow("empty-doc") << QStringLiteral("") << 0 << QStringLiteral("ds(") << QStringLiteral("") << false;
    QTest::newRow("readonly") << QStringLiteral("one") << 0 << QStringLiteral("ysiw)") << QStringLiteral("one") << true;
}

void TestVimInputHandler::surround()
{
    QFETCH(QString, input);
    QFETCH(int, column);
    QFETCH(QString, keys);
    QFETCH(QString, expected);
    QFETCH(bool, readOnly);
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText(input);
    editor.setReadOnly(readOnly);
    prepareEditor(editor);
    editor.setCursorPosition(0, column);
    handler.setEnabled(true);
    QApplication::clipboard()->setText("clipboard sentinel");
    QTest::keyClicks(&editor, keys);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), expected);
    QCOMPARE(QApplication::clipboard()->text(), QString("clipboard sentinel"));
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Normal);
    if(input != expected)
    {
        QTest::keyClick(&editor, Qt::Key_U);
        QCOMPARE(editor.text(), input);
        QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
        QCOMPARE(editor.text(), expected);
    }
}


void TestVimInputHandler::builtinCommands_data()
{
    QTest::addColumn<QString>("input"); QTest::addColumn<QString>("keys"); QTest::addColumn<QString>("expected");
    QTest::newRow("join") << "SELECT *\n  FROM test;" << "J" << "SELECT * FROM test;";
    QTest::newRow("raw join") << "a\n  b" << "gJ" << "a  b";
    QTest::newRow("count join") << "a\nb\nc\nd" << "3J" << "a b c\nd";
    QTest::newRow("visual join") << "a\nb\nc\nd" << "VjjJ" << "a b c\nd";
    QTest::newRow("forward find delete") << "abc,def,ghi" << "df," << "def,ghi";
    QTest::newRow("forward till delete") << "abc,def" << "dt," << ",def";
    QTest::newRow("backward find delete") << "abc,def" << "$dF," << "abcf";
    QTest::newRow("counted find") << "abc,def,ghi" << "d2f," << "ghi";
    QTest::newRow("digit target") << "abc2def" << "df2" << "def";
    QTest::newRow("uppercase word") << "select from" << "gUw" << "SELECT from";
    QTest::newRow("lowercase line") << "SELECT FROM" << "guu" << "select from";
    QTest::newRow("visual uppercase") << "abc def" << "viwU" << "ABC def";
    QTest::newRow("delete before") << "abc" << "$X" << "ac";
    QTest::newRow("named register") << "abc def" << "\"ayiw$\"ap" << "abc defabc";
    QTest::newRow("black hole") << "abc def" << "yiw\"_dwP" << "abcdef";
}

void TestVimInputHandler::builtinCommands()
{
    QFETCH(QString, input); QFETCH(QString, keys); QFETCH(QString, expected);
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText(input); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    QTest::keyClicks(&editor, keys); QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), expected);
}

void TestVimInputHandler::repeatAndMacro()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText("one two three"); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    QTest::keyClicks(&editor, "cwnew"); QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "w."); QCOMPARE(editor.text(), QString("new new three"));
    QTest::keyClicks(&editor, "u"); QCOMPARE(editor.text(), QString("new two three"));
    editor.setText("abc\ndef\nghi"); editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "qaxjq@a"); QCOMPARE(editor.text(), QString("bc\nef\nghi"));
}

void TestVimInputHandler::substitution()
{
    QsciScintilla editor; VimInputHandler handler(&editor);
    editor.setText("foo foo\nFOO bar"); handler.setEnabled(true);
    QVERIFY(handler.executeCommand("%s/foo/test/gi"));
    QCOMPARE(editor.text(), QString("test test\ntest bar"));
    editor.undo(); QCOMPARE(editor.text(), QString("foo foo\nFOO bar"));
    QVERIFY(!handler.executeCommand("%s/foo/test/c"));
    editor.setReadOnly(true); QVERIFY(!handler.executeCommand("%s/foo/test/g"));
}
