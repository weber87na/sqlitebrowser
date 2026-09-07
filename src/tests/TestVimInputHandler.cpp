#include "TestVimInputHandler.h"

#include "VimInputHandler.h"

#include <Qsci/qsciscintilla.h>

#include <QApplication>
#include <QClipboard>
#include <QtTest/QTest>
#include <QTemporaryFile>
#include <QLineEdit>

namespace
{
void prepareEditor(QsciScintilla& editor)
{
    editor.resize(640, 320);
    QCoreApplication::processEvents();
}
}

void TestVimInputHandler::changeHistory()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setUtf8(true);
    prepareEditor(editor);
    editor.setText("first\r\nsecond\r\nthird");
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "ix");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "Gix");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "ggg;");
    int line, column;
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 2); QCOMPARE(column, 0);
    QTest::keyClicks(&editor, "g;");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 0); QCOMPARE(column, 0);
    QTest::keyClicks(&editor, "g,");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 2); QCOMPARE(column, 0);
    QTest::keyClicks(&editor, "99g;");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 0); QCOMPARE(column, 0);
    // External edits adjust stored positions without creating new change entries.
    editor.insertAt("new\r\n", 0, 0);
    QTest::keyClicks(&editor, "g,");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 3); QCOMPARE(column, 0);
    QTest::keyClicks(&editor, "g;");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 1); QCOMPARE(column, 0);
}

void TestVimInputHandler::registerHistories_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("expected");
    QTest::newRow("counted line deletion") << "one\ntwo\nthree" << "2dd" << "1" << "one\ntwo\n";
    QTest::newRow("second previous deletion") << "one\ntwo\nthree" << "dddd" << "2" << "one\n";
    QTest::newRow("named deletion still rotates") << "one\ntwo\nthree" << "dd\"add" << "2" << "one\n";
    QTest::newRow("characterwise multiline deletion") << "one\ntwo\nthree" << "vjd" << "1" << "one\nt";
    QTest::newRow("small delete") << "abcdef" << "2x" << "-" << "ab";
    QTest::newRow("named small delete preserves previous") << "abcdef" << "x\"ax" << "-" << "a";
    QTest::newRow("small change") << "one two" << "cw" << "-" << "one";
    QTest::newRow("line change") << "one\ntwo" << "cc" << "1" << "one\n";
    QTest::newRow("yank survives delete") << "one two" << "yiwx" << "0" << "one";
    QTest::newRow("named yank preserves zero") << "one two" << "yiww\"ayiw" << "0" << "one";
    QTest::newRow("visual yank fills zero") << "abcdef" << "vly" << "0" << "ab";
    QTest::newRow("visual named register") << "abcdef" << "vl\"ay" << "a" << "ab";
    QTest::newRow("append named and unnamed") << "one two" << "\"ayiww\"Ayiw" << "\"" << "onetwo";
    QTest::newRow("black hole preserves yank") << "one two" << "yiw\"_x" << "0" << "one";
    QTest::newRow("black hole preserves small delete") << "abcdef" << "x\"_x" << "-" << "a";
    QTest::newRow("black hole preserves numbered") << "one\ntwo\nthree" << "dd\"_dd" << "1" << "one\n";
    QTest::newRow("black hole preserves clipboard") << "one two" << "yiw\"_x" << "+" << "one";
    QTest::newRow("black hole reads empty") << "abcdef" << "yiw" << "_" << "";
    QTest::newRow("utf8 register") << QString::fromUtf8("甲乙 丙丁") << "2x" << "-" << QString::fromUtf8("甲乙");
}

void TestVimInputHandler::registerHistories()
{
    QFETCH(QString, input);
    QFETCH(QString, keys);
    QFETCH(QString, name);
    QFETCH(QString, expected);
    QsciScintilla editor;
    editor.setUtf8(true);
    editor.setEolMode(QsciScintilla::EolUnix);
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    editor.setText(input);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, keys);
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setText("[]");
    editor.setCursorPosition(0, 1);
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, name);
    QCOMPARE(editor.text(), "[" + expected + "]");
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Insert);
}

void TestVimInputHandler::numberedRegisterRotation()
{
    QsciScintilla editor;
    editor.setEolMode(QsciScintilla::EolUnix);
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    QString source;
    for(int i = 0; i < 12; ++i) source += QString::number(i) + "\n";
    editor.setText(source);
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    for(int i = 0; i < 10; ++i) QTest::keyClicks(&editor, "dd");
    for(int i = 1; i <= 9; ++i)
    {
        editor.setText("end");
        editor.setCursorPosition(0, 0);
        QTest::keyClicks(&editor, "\"" + QString::number(i) + "P");
        QCOMPARE(editor.text(), QString::number(10-i) + "\nend");
    }
    // A small deletion does not evict an entry from numbered history.
    editor.setText("abc");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "x");
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "\"1P");
    QCOMPARE(editor.text(), QString("9\nend"));
}

void TestVimInputHandler::registerPasteShapes()
{
    QsciScintilla editor;
    editor.setEolMode(QsciScintilla::EolWindows);
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    editor.setText("one\r\ntwo\r\nthree");
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "\"ayyj\"Ayy");
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "\"a2P");
    QCOMPARE(editor.text(), QString("one\r\ntwo\r\none\r\ntwo\r\nend"));
    // The explicit unnamed register must use the same linewise contents.
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "\"\"p");
    QCOMPARE(editor.text(), QString("end\r\none\r\ntwo"));
    // Escape cancels a register prefix before an unrelated command.
    QTest::keyClicks(&editor, "\"a");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QApplication::clipboard()->setText("X");
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "p");
    QCOMPARE(editor.text(), QString("eXnd"));
}

void TestVimInputHandler::blockRegisterHistory()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    editor.setText("abc\ndef");
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClicks(&editor, "jly");
    // A later small delete must not discard the shape of register 0.
    QTest::keyClicks(&editor, "x");
    editor.setText("12\n34");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "\"0P");
    QCOMPARE(editor.text(), QString("ab12\nde34"));
    // Black-hole block deletion also preserves the current register's shape.
    editor.setText("word");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "yiw");
    editor.setText("abc\ndef");
    editor.setCursorPosition(0, 0);
    QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClicks(&editor, "jl\"_d");
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "P");
    QCOMPARE(editor.text(), QString("wordend"));
}

void TestVimInputHandler::insertRegisterClipboardAndCancel()
{
    QsciScintilla editor;
    editor.setUtf8(true);
    editor.setEolMode(QsciScintilla::EolWindows);
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);
    QApplication::clipboard()->setText(QString::fromUtf8("甲\n乙"));
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "+");
    QCOMPARE(editor.text(), QString::fromUtf8("甲\r\n乙"));
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Insert);
    QTest::keyClicks(&editor, "!");
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "="); // Unsupported registers do not leak into text.
    QCOMPARE(editor.text(), QString::fromUtf8("甲\r\n乙!"));
    // '*' aliases the clipboard on Windows and uses the primary selection on X11.
    const auto mode = QApplication::clipboard()->supportsSelection() ? QClipboard::Selection : QClipboard::Clipboard;
    QApplication::clipboard()->setText("S", mode);
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "*");
    QCOMPARE(editor.text(), QString::fromUtf8("甲\r\n乙!S"));
    // An empty explicit clipboard register must not paste stale internal data.
    QApplication::clipboard()->clear();
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "+");
    QCOMPARE(editor.text(), QString::fromUtf8("甲\r\n乙!S"));
}

void TestVimInputHandler::insertRegisterCountRepeatUndo()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    editor.setText("foo");
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "yiw");
    editor.setText("");
    QTest::keyClicks(&editor, "2i");
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "0");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), QString("foofoo"));
    QTest::keyClicks(&editor, ".");
    QCOMPARE(editor.text(), QString("foofofoofooo"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString("foofoo"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString());
}

void TestVimInputHandler::readOnlyRegisterOperations()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    editor.setText("abc");
    editor.setCursorPosition(0, 0);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "x");
    editor.setReadOnly(true);
    QTest::keyClicks(&editor, "dd");
    QCOMPARE(editor.text(), QString("bc"));
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_R, Qt::ControlModifier);
    QTest::keyClicks(&editor, "-");
    QCOMPARE(editor.text(), QString("bc"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setReadOnly(false);
    editor.setText("end");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "\"-P");
    QCOMPARE(editor.text(), QString("aend"));
}

void TestVimInputHandler::lineStartMotions()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("  one\r\n    two\r\n three\r\nlast");
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(0, 3);
    QTest::keyClicks(&editor, "2+");
    int line, column;
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 2); QCOMPARE(column, 1);
    QTest::keyClicks(&editor, "-");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 1); QCOMPARE(column, 4);
    QTest::keyClick(&editor, Qt::Key_Return);
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 2); QCOMPARE(column, 1);
    QTest::keyClicks(&editor, "gg2_");
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 1); QCOMPARE(column, 4);
    QTest::keyClicks(&editor, "ggd2_");
    QCOMPARE(editor.text(), QString(" three\r\nlast"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString("  one\r\n    two\r\n three\r\nlast"));
    QTest::keyClicks(&editor, "ggd");
    QTest::keyClick(&editor, Qt::Key_Return);
    QCOMPARE(editor.text(), QString(" three\r\nlast"));
    QTest::keyClicks(&editor, "Gd-");
    QCOMPARE(editor.text(), QString());
}

void TestVimInputHandler::backwardEndOperators()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setUtf8(true);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setText("one two three");
    editor.setCursorPosition(0, 8);
    QTest::keyClicks(&editor, "dge");
    QCOMPARE(editor.text(), QString("one twhree"));
    QTest::keyClicks(&editor, "u");
    editor.setCursorPosition(0, 8);
    QTest::keyClicks(&editor, "d2ge");
    QCOMPARE(editor.text(), QString("onhree"));
    editor.setText("one.two three");
    editor.setCursorPosition(0, 4);
    QTest::keyClicks(&editor, "dgE");
    QCOMPARE(editor.text(), QString("wo three"));
    editor.setText(QString::fromUtf8("甲乙 丙丁"));
    editor.setCursorPosition(0, 3);
    QTest::keyClicks(&editor, "dge");
    QCOMPARE(editor.text(), QString::fromUtf8("甲丁"));
    editor.setText("one two");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "dge");
    QCOMPARE(editor.text(), QString("one two"));
    QTest::keyClicks(&editor, "dgE");
    QCOMPARE(editor.text(), QString("one two"));
    QTest::keyClicks(&editor, "d-d+d2_");
    QCOMPARE(editor.text(), QString("one two"));
    QTest::keyClicks(&editor, "d_");
    QCOMPARE(editor.text(), QString());
    editor.setText("one\r\n\r\n two");
    editor.setCursorPosition(2, 0);
    QTest::keyClicks(&editor, "ge");
    int line, column;
    editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 1); QCOMPARE(column, 0);
    editor.setCursorPosition(2, 0);
    QTest::keyClicks(&editor, "dgE");
    QCOMPARE(editor.text(), QString("one\r\ntwo"));
}

void TestVimInputHandler::insertControlDeletion()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);
    QTest::keyClicks(&editor, "iabc");
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("ab"));
    QTest::keyClicks(&editor, "z"); // Flush the pending custom-mapping prefix before deleting.
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString());
    QVERIFY(handler.mode() == VimInputHandler::Mode::Insert);
    QTest::keyClicks(&editor, "new");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString());
    editor.setText("one\r\ntwo");
    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("onetwo"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setText("prefix suffix");
    editor.setCursorPosition(0, 7);
    QTest::keyClicks(&editor, "iadded");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("prefix suffix"));
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("suffix"));
    editor.setReadOnly(true);
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("suffix"));
}

void TestVimInputHandler::insertCompletionCycles()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    const QString original = "apricot apple apple\n\napply apogee";
    editor.setText(original);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "iap");
    for(const QString& candidate : {QString("apply"), QString("apogee"), QString("apricot"), QString("apple"), QString("ap")})
    {
        QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(editor.text(1), candidate + "\n");
    }
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple\n"));
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("ap\n"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), original); // All insertion and completion cycles undo together.

    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "iap");
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple\n"));
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apricot\n"));
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple\n"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    // A repeated word after the caret must not hide the nearest backward match.
    editor.setText("apricot apple\nap\napple");
    editor.setCursorPosition(1, 1);
    QTest::keyClicks(&editor, "A");
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple\n"));
}

void TestVimInputHandler::insertCompletionUnicodeAndSafety()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setUtf8(true);
    editor.setText(QString::fromUtf8("測試 測量 測試\r\n測"));
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "A");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("測試"));
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("測量"));
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("測"));
    editor.setReadOnly(true);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("測"));
    editor.setReadOnly(false);
    editor.setSelection(0, 0, 0, 2);
    const QString before = editor.text();
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(), before);
    QVERIFY(editor.hasSelectedText());
    QTest::keyClick(&editor, Qt::Key_Escape);

    // A supplementary Unicode letter occupies four UTF-8 bytes and two UTF-16 units.
    editor.setText(QString::fromUtf8("𠀀甲 𠀀乙\n𠀀"));
    editor.SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION, editor.text().toUtf8().size());
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("𠀀甲"));
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString::fromUtf8("𠀀"));
}

void TestVimInputHandler::insertCompletionResets()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("apple apply banana\nap");
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(1, 1);
    QTest::keyClicks(&editor, "A");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple"));
    QTest::keyClicks(&editor, "_");
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple_"));

    editor.setText("apple apply banana\nap ba");
    editor.setCursorPosition(1, 2);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple ba"));
    editor.setCursorPosition(1, 8);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apple banana"));

    editor.setText("apex apricot\nap");
    editor.setCursorPosition(1, 2);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apex"));
    editor.insertAt("X", 0, 0);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("apex"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    // Empty prefixes complete whole words, and the original empty input is recoverable.
    editor.setText("alpha beta\n");
    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString("alpha"));
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QCOMPARE(editor.text(1), QString());
}

void TestVimInputHandler::insertCompletionDotRepeat()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    const QString original = "\napple apply\n\napricot";
    editor.setText(original);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "iap");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClicks(&editor, "_id");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(0), QString("apply_id\n"));
    QTest::keyClicks(&editor, "2j.");
    QCOMPARE(editor.text(2), QString("apply_id\n")); // Local completion would instead choose apricot.
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(2), QString("\n"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), original);

    editor.setText("\napple\n\napricot");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "2iap");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(0), QString("appleapple\n"));
    QTest::keyClicks(&editor, "2j.");
    QCOMPARE(editor.text(2), QString("appleapple\n"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(2), QString("\n"));
}

void TestVimInputHandler::insertCompletionRepeatEdges()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setUtf8(true);
    editor.setText("\napple\n\napricot");
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "iap");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_P, Qt::ControlModifier); // Restore the original prefix.
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "2j.");
    QCOMPARE(editor.text(2), QString("ap\n"));

    // Completion of a prefix that predates insertion repeats just the inserted suffix.
    editor.setText(QString::fromUtf8("測\n測試 測量\n\n測字"));
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "A");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(0), QString::fromUtf8("測量\n"));
    QTest::keyClicks(&editor, "2j.");
    QCOMPARE(editor.text(2), QString::fromUtf8("量\n"));

    // No-match completion does not start completing when a later edit adds a candidate.
    editor.setText("");
    QTest::keyClicks(&editor, "iap");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setText("\napple");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, ".");
    QCOMPARE(editor.text(0), QString("ap\n"));
    editor.setReadOnly(true);
    const QString before = editor.text();
    QTest::keyClicks(&editor, ".");
    QCOMPARE(editor.text(), before);
}

void TestVimInputHandler::insertCompletionMacroIsDynamic()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setText("\napple\n\napricot");
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "qaiap");
    QTest::keyClick(&editor, Qt::Key_N, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "q");
    QCOMPARE(editor.text(0), QString("apple\n"));
    QTest::keyClicks(&editor, "2j@a");
    QCOMPARE(editor.text(2), QString("apricot\n")); // Macros intentionally replay completion keys.
}

void TestVimInputHandler::insertBackspaceAnchors()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setText("one\r\ntwo");
    editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "iq");
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QTest::keyClicks(&editor, "added");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("onetwo"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString("one\r\ntwo"));

    editor.setText("prefix suffix");
    editor.setCursorPosition(0, 7);
    QTest::keyClicks(&editor, "iadded");
    for(int n = 0; n < 7; ++n) QTest::keyClick(&editor, Qt::Key_Left);
    QTest::keyClicks(&editor, "newtext");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("prefix addedsuffix"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    editor.setText("prefix suffix");
    editor.setCursorPosition(0, 7);
    QTest::keyClicks(&editor, "i");
    QTest::keyClick(&editor, Qt::Key_Right);
    QTest::keyClick(&editor, Qt::Key_Right);
    QTest::keyClicks(&editor, "abc");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("prefix suffix"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    // Same-line Backspace retains Vim's original insertion column.
    editor.setText("prefix suffix");
    editor.setCursorPosition(0, 7);
    QTest::keyClicks(&editor, "iadded");
    for(int n = 0; n < 7; ++n) QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QTest::keyClicks(&editor, "longadded");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("prefilosuffix"));
}

void TestVimInputHandler::replaceBackspacing()
{
    QsciScintilla editor;
    VimInputHandler handler(&editor);
    editor.setUtf8(true);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setText("abcdef");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "RXY");
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("Xbcdef"));
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("abcdef"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    editor.setText("abcdef");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "RXY");
    QTest::keyClick(&editor, Qt::Key_Backspace);
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), QString("Xbcdef"));
    editor.setCursorPosition(0, 2);
    QTest::keyClicks(&editor, ".");
    QCOMPARE(editor.text(), QString("XbXdef"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString("Xbcdef"));
    QTest::keyClicks(&editor, "u");
    QCOMPARE(editor.text(), QString("abcdef"));

    editor.setText(QString::fromUtf8("甲乙丙"));
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "Rxy");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString::fromUtf8("甲乙丙"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    editor.setText("ab");
    editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "Rxyz");
    QTest::keyClick(&editor, Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("ab"));
    QTest::keyClick(&editor, Qt::Key_Escape);

    editor.setText("abcdef");
    editor.setCursorPosition(0, 2);
    QTest::keyClicks(&editor, "R");
    QTest::keyClick(&editor, Qt::Key_H, Qt::ControlModifier);
    QTest::keyClicks(&editor, "Z");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), QString("aZcdef"));
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

void TestVimInputHandler::paragraphAndSentenceObjects_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("line");
    QTest::addColumn<int>("column");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("insert");
    const QString paragraphs = "one\ncontinued\n\n \t\ntwo\n\nthree";
    QTest::newRow("inner-paragraph") << paragraphs << 1 << 2 << "dip" << "\n \t\ntwo\n\nthree" << false;
    QTest::newRow("around-paragraph") << paragraphs << 1 << 2 << "dap" << "two\n\nthree" << false;
    QTest::newRow("inner-count-includes-blank-block") << paragraphs << 0 << 1 << "d2ip" << "two\n\nthree" << false;
    QTest::newRow("inner-three-parts") << paragraphs << 0 << 1 << "d3ip" << "\nthree" << false;
    QTest::newRow("around-two-paragraphs") << paragraphs << 0 << 1 << "2dap" << "three" << false;
    QTest::newRow("operator-and-object-counts") << "one\n\ntwo\n\nthree\n\nfour\n\nfive" << 0 << 0 << "2d2ap" << "five" << false;
    QTest::newRow("inner-blank-paragraph") << paragraphs << 2 << 0 << "dip" << "one\ncontinued\ntwo\n\nthree" << false;
    QTest::newRow("around-blank-paragraph") << paragraphs << 2 << 0 << "dap" << "one\ncontinued\n\nthree" << false;
    QTest::newRow("around-last-paragraph") << "one\n\ntwo" << 2 << 1 << "dap" << "one" << false;
    QTest::newRow("final-eol-is-not-blank-paragraph") << "one\n\ntwo\n" << 2 << 1 << "dap" << "one\n" << false;
    QTest::newRow("paragraph-change") << "one\n\ntwo" << 0 << 1 << "cip" << "\n\ntwo" << true;
    QTest::newRow("paragraph-uppercase") << "one\ncontinued\n\ntwo" << 1 << 2 << "gUip" << "ONE\nCONTINUED\n\ntwo" << false;
    QTest::newRow("paragraph-macro") << "one\n.LP\ntwo\n.LP\nthree" << 2 << 1 << "dip" << "one\n.LP\nthree" << false;
    QTest::newRow("paragraph-crlf-unicode") << QString::fromUtf8("甲乙\r\n丙丁\r\n\r\n下一段") << 1 << 1 << "dap" << QString::fromUtf8("下一段") << false;
    QTest::newRow("inner-sentence") << "One.  Two!  Three?" << 0 << 7 << "dis" << "One.    Three?" << false;
    QTest::newRow("around-sentence") << "One.  Two!  Three?" << 0 << 7 << "das" << "One.  Three?" << false;
    QTest::newRow("inner-sentence-whitespace") << "One.  Two!" << 0 << 4 << "dis" << "One.Two!" << false;
    QTest::newRow("around-sentence-whitespace") << "One.  Two!  Three?" << 0 << 4 << "das" << "One.  Three?" << false;
    QTest::newRow("inner-two-sentence-parts") << "One.  Two!  Three?" << 0 << 1 << "d2is" << "Two!  Three?" << false;
    QTest::newRow("inner-three-sentence-parts") << "One.  Two!  Three?" << 0 << 1 << "d3is" << "  Three?" << false;
    QTest::newRow("around-two-sentences") << "One.  Two!  Three?" << 0 << 1 << "d2as" << "Three?" << false;
    QTest::newRow("around-last-sentence") << "One.  Two!" << 0 << 7 << "das" << "One." << false;
    QTest::newRow("sentence-closing-punctuation") << "He said (\"Go!\")  Next." << 0 << 10 << "dis" << "  Next." << false;
    QTest::newRow("decimal-is-not-sentence-boundary") << "Value 3.14 is fine. Next." << 0 << 9 << "das" << "Next." << false;
    QTest::newRow("sentence-spans-lines") << "One continues\non this line. Next." << 1 << 3 << "cis" << " Next." << true;
    QTest::newRow("sentence-at-line-end") << "One.\r\nTwo." << 0 << 1 << "dis" << "Two." << false;
    QTest::newRow("sentence-preserves-final-eol") << "One.\r\n" << 0 << 1 << "das" << "\r\n" << false;
    QTest::newRow("sentence-paragraph-boundary") << "One\n\nTwo" << 0 << 1 << "dis" << "\nTwo" << false;
    QTest::newRow("sentence-empty-line") << "One.\n\nTwo." << 1 << 0 << "dis" << "One.\nTwo." << false;
    QTest::newRow("sentence-unicode") << QString::fromUtf8("中文句子!  下一句.") << 0 << 2 << "das" << QString::fromUtf8("下一句.") << false;
    QTest::newRow("empty-paragraph") << "" << 0 << 0 << "dip" << "" << false;
    QTest::newRow("empty-sentence") << "" << 0 << 0 << "dis" << "" << false;
}

void TestVimInputHandler::paragraphAndSentenceObjects()
{
    QFETCH(QString, input);
    QFETCH(int, line);
    QFETCH(int, column);
    QFETCH(QString, keys);
    QFETCH(QString, expected);
    QFETCH(bool, insert);
    QsciScintilla editor;
    editor.setUtf8(true);
    VimInputHandler handler(&editor);
    editor.setText(input);
    prepareEditor(editor);
    editor.setCursorPosition(line, column);
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

void TestVimInputHandler::paragraphAndSentenceSelections()
{
    QsciScintilla editor;
    editor.setUtf8(true);
    VimInputHandler handler(&editor);
    prepareEditor(editor);
    handler.setEnabled(true);
    editor.setText("one\ncontinued\n\ntwo\n\nthree");
    editor.setCursorPosition(1, 2);
    QTest::keyClicks(&editor, "vip");
    QCOMPARE(handler.mode(), VimInputHandler::Mode::VisualLine);
    QCOMPARE(editor.selectedText(), QString("one\ncontinued\n"));
    QTest::keyClicks(&editor, "ip");
    QCOMPARE(editor.selectedText(), QString("one\ncontinued\n\n"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setText("One.  Two!  Three?");
    editor.setCursorPosition(0, 1);
    QTest::keyClicks(&editor, "Vas");
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Visual);
    QCOMPARE(editor.selectedText(), QString("One.  "));
    QTest::keyClicks(&editor, "as");
    QCOMPARE(editor.selectedText(), QString("One.  Two!"));
    QTest::keyClick(&editor, Qt::Key_Escape);
    editor.setText("one\n\nlast");
    editor.setCursorPosition(0, 1);
    QTest::keyClicks(&editor, "yipGp");
    QCOMPARE(editor.text(), QString("one\n\nlast\none"));
    editor.setReadOnly(true);
    editor.setCursorPosition(0, 1);
    QTest::keyClicks(&editor, "dap");
    QCOMPARE(editor.text(), QString("one\n\nlast\none"));
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
    QTest::newRow("delete final word") << "abc def" << "wdw" << "abc ";
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
    editor.setReadOnly(false); editor.setText("keep\nremove\nkeep\nremove");
    QVERIFY(handler.executeCommand("g/remove/d")); QCOMPARE(editor.text(), QString("keep\nkeep\n"));
    editor.undo(); QCOMPARE(editor.text(), QString("keep\nremove\nkeep\nremove"));
}


void TestVimInputHandler::exLineCommands_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("expected");
    QTest::newRow("sort default entire buffer") << "c\na\nb" << "sort" << "a\nb\nc";
    QTest::newRow("sort reverse unique CRLF") << "b\r\na\r\nb\r\nc\r\n" << "%sort! u" << "c\r\nb\r\na\r\n";
    QTest::newRow("sort unique unterminated") << "b\na\nb" << "sort u" << "a\nb";
    QTest::newRow("sort preserves mixed boundaries") << "c\r\na\nb" << "sort" << "a\r\nb\nc";
    QTest::newRow("sort bounded range") << "outside\nc\nb\na\nlast" << "2,$-1sort" << "outside\na\nb\nc\nlast";
    QTest::newRow("sort blank line is real") << "b\n\na\n" << "%sor" << "\na\nb\n";
    QTest::newRow("sort unicode") << QString::fromUtf8("甲\n丙\n乙") << "sort" << QString::fromUtf8("丙\n乙\n甲");
    QTest::newRow("substitute relative range") << "foo\r\nfoo\r\nfoo\r\n" << ".+1,$s/foo/bar/g" << "foo\r\nbar\r\nbar\r\n";
    QTest::newRow("substitute one address") << "foo\nfoo\nfoo" << "2s/foo/bar/" << "foo\nbar\nfoo";
    QTest::newRow("delete numeric range") << "one\r\ntwo\r\nthree\r\nfour" << ": 2,3 delete" << "one\r\nfour";
    QTest::newRow("delete relative offset") << "one\ntwo\nthree" << "+d" << "one\nthree";
    QTest::newRow("delete semicolon relative address") << "one\ntwo\nthree\nfour" << "2;+1d" << "one\nfour";
    QTest::newRow("delete entire buffer") << "one\r\ntwo\r\n" << "%d" << "";
    QTest::newRow("global limited range") << "drop\ndrop\nkeep\ndrop" << "2,3g/drop/d" << "drop\nkeep\ndrop";
}

void TestVimInputHandler::exLineCommands()
{
    QFETCH(QString, input); QFETCH(QString, command); QFETCH(QString, expected);
    QsciScintilla editor; VimInputHandler handler(&editor);
    editor.setUtf8(true); editor.setText(input); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    editor.SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    editor.setReadOnly(true);
    QApplication::clipboard()->setText("unchanged");
    QVERIFY(!handler.executeCommand(command));
    QCOMPARE(editor.text(), input);
    QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
    QVERIFY(!editor.SendScintilla(QsciScintillaBase::SCI_CANUNDO));
    editor.setReadOnly(false);
    QVERIFY(handler.executeCommand(command));
    QCOMPARE(editor.text(), expected);
    if(input != expected)
    {
        editor.undo(); QCOMPARE(editor.text(), input);
        QVERIFY(!editor.SendScintilla(QsciScintillaBase::SCI_CANUNDO));
        editor.redo(); QCOMPARE(editor.text(), expected);
    }
}

void TestVimInputHandler::exRangesAndMarks()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setEolMode(QsciScintilla::EolWindows);
    editor.setText("one\r\ntwo\r\nthree\r\nfour\r\n");
    handler.setEnabled(true); editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "ma");
    editor.setCursorPosition(3, 0);
    editor.setReadOnly(true);
    QVERIFY(handler.executeCommand("'a+1,$yank"));
    QCOMPARE(QApplication::clipboard()->text(), QString("three\r\nfour\r\n"));
    QCOMPARE(editor.text(), QString("one\r\ntwo\r\nthree\r\nfour\r\n"));
    QVERIFY(handler.executeCommand("'a"));
    int line, column; editor.getCursorPosition(&line, &column);
    QCOMPARE(line, 1); QCOMPARE(column, 0);
    QVERIFY(handler.executeCommand("$"));
    editor.getCursorPosition(&line, &column); QCOMPARE(line, 3);
    // The final EOL must not become a fifth, phantom Ex line.
    QVERIFY(!handler.executeCommand("5y"));
    editor.setCursorPosition(2, 0);
    QVERIFY(handler.executeCommand("1,+1y"));
    QCOMPARE(QApplication::clipboard()->text(), QString("one\r\ntwo\r\nthree\r\nfour\r\n"));
    editor.setCursorPosition(2, 0);
    QVERIFY(handler.executeCommand("1;+1y"));
    QCOMPARE(QApplication::clipboard()->text(), QString("one\r\ntwo\r\n"));
    editor.setReadOnly(false);
    QVERIFY(handler.executeCommand("'a,$-1s/two/TWO/"));
    QCOMPARE(editor.text(), QString("one\r\nTWO\r\nthree\r\nfour\r\n"));
    editor.undo(); QCOMPARE(editor.text(), QString("one\r\ntwo\r\nthree\r\nfour\r\n"));
}

void TestVimInputHandler::exRejectsInvalidCommands()
{
    QsciScintilla editor; VimInputHandler handler(&editor);
    editor.setText("three\r\ntwo\r\none\r\n"); handler.setEnabled(true);
    editor.setCursorPosition(1, 1);
    editor.SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    QApplication::clipboard()->setText("unchanged");
    const QStringList commands = {
        "0d", "4d", "3,1d", "1,d", "1,2,3d", "%1d", "'zd", "'<,'>d", "'",
        "999999999999999999999d", "1+999999999999999999999d", ".-2d", "$+1d",
        "sort n", "sort i", "sort uu", "sort! u garbage", "sort /pattern/", "sort!!",
        "1,2d | sort", "1,2delete extra", "1,2y extra", "1,2!sort", "1,2w",
        "1,2s/two/changed/c", "1,2s/two/changed/g/extra", "1,2s/[/changed/",
        "1,2s|two|changed|", "1,2global/two/d", "1,2s/two/changed\\"
    };
    for(const QString& command : commands)
    {
        QVERIFY2(!handler.executeCommand(command), qPrintable(command));
        QCOMPARE(editor.text(), QString("three\r\ntwo\r\none\r\n"));
        QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
        QCOMPARE(int(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)), 8);
        QVERIFY(!editor.SendScintilla(QsciScintillaBase::SCI_CANUNDO));
    }
}

void TestVimInputHandler::exVisualRange()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText("outside\r\nc\r\na\r\nlast\r\n");
    handler.setEnabled(true); editor.setCursorPosition(1, 0);
    QTest::keyClicks(&editor, "Vj:");
    QLineEdit* commandLine = editor.findChild<QLineEdit*>();
    QVERIFY(commandLine);
    QCOMPARE(commandLine->text(), QString("'<,'>"));
    QTest::keyClicks(commandLine, "sort"); QTest::keyClick(commandLine, Qt::Key_Return);
    QCOMPARE(editor.text(), QString("outside\r\na\r\nc\r\nlast\r\n"));
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Normal);
    editor.undo(); QCOMPARE(editor.text(), QString("outside\r\nc\r\na\r\nlast\r\n"));
    // Saved visual marks remain available after leaving Visual mode.
    editor.setCursorPosition(1, 0); QTest::keyClicks(&editor, "Vj");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QVERIFY(handler.executeCommand("'<,'>y"));
    QCOMPARE(QApplication::clipboard()->text(), QString("c\r\na\r\n"));
}


void TestVimInputHandler::blockEditing()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText("abc\ndef\nghi"); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClicks(&editor, "jjld"); QCOMPARE(editor.text(), QString("c\nf\ni"));
    QTest::keyClicks(&editor, "u"); QCOMPARE(editor.text(), QString("abc\ndef\nghi"));
    editor.setCursorPosition(0, 0);
    QTest::keyClick(&editor, Qt::Key_V, Qt::ControlModifier); QTest::keyClicks(&editor, "jjI-- ");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), QString("-- abc\n-- def\n-- ghi"));
    QTest::keyClicks(&editor, "u"); QCOMPARE(editor.text(), QString("abc\ndef\nghi"));
}

void TestVimInputHandler::configMappings()
{
    QTemporaryFile config; QVERIFY(config.open());
    config.write(R"({"leader":",","timeoutMs":300,"shiftWidth":2,"mappings":{"i:jk":"<Esc>","n:Q":"gUw"}})"); config.flush();
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    QVERIFY(handler.loadConfig(config.fileName())); handler.setEnabled(true);
    QTest::keyClicks(&editor, "iselectjkQ"); QCOMPARE(editor.text(), QString("selecT"));
    QCOMPARE(handler.mode(), VimInputHandler::Mode::Normal);
    QCOMPARE(editor.indentationWidth(), 2);
}

void TestVimInputHandler::findRepeatAndMarks()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText("a,b,c,d\nsecond\nthird"); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    QTest::keyClicks(&editor, "t,;");
    QCOMPARE(int(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)), 2);
    QTest::keyClicks(&editor, "maG`a");
    QCOMPARE(int(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)), 2);
    QTest::keyClicks(&editor, "G"); QTest::keyClick(&editor, Qt::Key_O, Qt::ControlModifier);
    QCOMPARE(int(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)), 2);
}


void TestVimInputHandler::countedInsert()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor); handler.setEnabled(true);
    QTest::keyClicks(&editor, "3iabc"); QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.text(), QString("abcabcabc"));
    QTest::keyClicks(&editor, "."); QCOMPARE(editor.text(), QString("abcabcababcabcabcc"));
    QTest::keyClicks(&editor, "2u"); QCOMPARE(editor.text(), QString());
}

void TestVimInputHandler::numericAndSqlMappings()
{
    QsciScintilla editor; VimInputHandler handler(&editor); prepareEditor(editor);
    editor.setText("LIMIT 10"); editor.setCursorPosition(0, 0); handler.setEnabled(true);
    QTest::keyClicks(&editor, "2"); QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(editor.text(), QString("LIMIT 12"));
    QTest::keyClick(&editor, Qt::Key_X, Qt::ControlModifier); QCOMPARE(editor.text(), QString("LIMIT 11"));
    editor.setText("  one\n  two"); editor.setCursorPosition(0, 0);
    QTest::keyClicks(&editor, "Vj;q"); QCOMPARE(editor.text(), QString("  + 'one'\n  + 'two'"));
    editor.setCursorPosition(0, 0); QTest::keyClicks(&editor, "Vj;h"); QCOMPARE(editor.text(), QString("  one\n  two"));
}
