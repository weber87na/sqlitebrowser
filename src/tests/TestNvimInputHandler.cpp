#include "NvimInputHandler.h"
#include "NvimMessagePack.h"
#include <Qsci/qsciscintilla.h>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <QSignalSpy>

class TestNvimInputHandler : public QObject {
    Q_OBJECT
private slots:
    void codec();
    void commands_data();
    void commands();
    void motions();
    void configAndInput();
    void vimrc();
    void externalEdits();
    void readonly();
    void scrolling();
    void saveBurst();
    void emptyEditor();
};
void TestNvimInputHandler::codec()
{
    const QVariantList original{0, 42, QString("中文"), QVariantList{true, -1000, QVariantMap{{"key","value"}}}};
    const auto emptyBytes=NvimMessagePack::encode(QString());
    NvimMessagePack::Reader emptyReader(emptyBytes);
    const auto emptyValue=emptyReader.value();
    QVERIFY(emptyValue.isValid()); QCOMPARE(emptyValue.type(),QVariant::String);
    QVERIFY(emptyValue.toString().isEmpty());
    const auto bytes=NvimMessagePack::encode(original);
    for(int i=0;i<bytes.size();++i) {
        const auto fragment=bytes.left(i); NvimMessagePack::Reader reader(fragment); reader.value();
        QVERIFY(!reader.complete); QVERIFY(!reader.invalid);
    }
    NvimMessagePack::Reader reader(bytes);
    QCOMPARE(reader.value().toList(),original); QVERIFY(reader.complete); QVERIFY(!reader.invalid);
}
void TestNvimInputHandler::commands_data()
{
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("keys");
    QTest::addColumn<QString>("after");
#define ROW(name,b,k,a) QTest::newRow(name) << QString::fromUtf8(b) << QString::fromUtf8(k) << QString::fromUtf8(a)
    ROW("join", "one\n  two", "J", "one two");
    ROW("raw-join", "one\n  two", "gJ", "one  two");
    ROW("visual-join", "one\ntwo\nthree", "VjJ", "one two\nthree");
    ROW("dot", "one two three", "dw.", "three");
    ROW("dot-insert", "one\ntwo", "A!<Esc>j.", "one!\ntwo!");
    ROW("find-delete", "abc,def;ghi", "dt;", ";ghi");
    ROW("find-back", "abc,def", "$dF,", "abcf");
    ROW("repeat-find", "a,b,c,d", "f,;x", "a,bc,d");
    ROW("X", "abc", "lX", "bc");
    ROW("S", "one\ntwo", "Snew<Esc>", "new\ntwo");
    ROW("R", "abcd", "RXY<Esc>", "XYcd");
    ROW("upper", "hello world", "gUiw", "HELLO world");
    ROW("lower", "HELLO world", "guiw", "hello world");
    ROW("indent", "one\ntwo", ":set sw=2 et<CR>>>", "  one\ntwo");
    ROW("unindent", "  one\ntwo", ":set sw=2 et<CR><LT><LT>", "one\ntwo");
    ROW("block-delete", "abcd\nefgh\nijkl", "<C-v>jld", "cd\ngh\nijkl");
    ROW("gv", "one two", "viw<Esc>gvd", " two");
    ROW("visual-end", "abcd", "vllohd", "d");
    ROW("mark", "one\ntwo", "maj0'aD", "\ntwo");
    ROW("macro", "one\ntwo", "qaA!<Esc>qj@a", "one!\ntwo!");
    ROW("register", "one two", "\"ayiw$\"ap", "one twoone");
    ROW("blackhole", "one two", "\"_dw", "two");
    ROW("substitute", "foo foo\nfoo", ":%s/foo/bar/g<CR>", "bar bar\nbar");
    ROW("regex", "item12 item34", ":%s/\\d\\+/X/g<CR>", "itemX itemX");
    ROW("global", "keep\nremove\nkeep", ":g/remove/d<CR>", "keep\nkeep");
    ROW("surround", "one two", "ysiw\"", "\"one\" two");
    ROW("change-surround", "\"one\"", "cs\"'", "'one'");
    ROW("delete-surround", "(one)", "ds)", "one");
    ROW("surround-dot", "one two", "ysiw\"$b.", "\"one\" \"two\"");
    ROW("clipboard-line", "one\ntwo", "yyp", "one\none\ntwo");
    ROW("crlf", "one\r\ntwo\r\n", "J", "one two\r\n");
#undef ROW
}
void TestNvimInputHandler::commands()
{
    QFETCH(QString,before); QFETCH(QString,keys); QFETCH(QString,after);
    QTemporaryDir config; QsciScintilla editor; editor.resize(800,400); editor.show();
    editor.setEolMode(before.contains("\r\n") ? QsciScintilla::EolWindows : QsciScintilla::EolUnix);
    editor.setText(before); editor.setCursorPosition(0,0);
    NvimInputHandler nvim(&editor); QSignalSpy errors(&nvim,&NvimInputHandler::failed);
    QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY_WITH_TIMEOUT(nvim.isReady(),10000);
    nvim.input(keys);
    QTRY_COMPARE_WITH_TIMEOUT(editor.text(),after,7000);
    QCOMPARE(errors.count(),0);
}
void TestNvimInputHandler::motions()
{
    QTemporaryDir config; QsciScintilla editor; editor.resize(800,400); editor.show();
    editor.setText("one two one\n\nnext paragraph. Second sentence!\nlast");
    editor.setCursorPosition(0,0); NvimInputHandler nvim(&editor);
    QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady());
    nvim.input("*");
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS),8L);
    nvim.input("#");
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS),0L);
    nvim.input("}");
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
        editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)),1L);
    nvim.input("j)");
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS),29L);
}
void TestNvimInputHandler::configAndInput()
{
    QTemporaryDir config; QFile init(config.path()+"/init.lua"); QVERIFY(init.open(QIODevice::WriteOnly));
    init.write("vim.keymap.set('n','Q','A!<Esc>')\n"); init.close();
    QsciScintilla editor; editor.setEolMode(QsciScintilla::EolUnix); editor.setText("one\ntwo");
    editor.resize(800,400); editor.show(); editor.setCursorPosition(0,0);
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady()); QTest::keyClick(&editor,Qt::Key_J,Qt::ShiftModifier);
    QTRY_COMPARE(editor.text(),QString("one two"));
    QTest::keyClick(&editor,Qt::Key_Q,Qt::ShiftModifier);
    QTRY_COMPARE(editor.text(),QString("one two!"));
    QTest::keyClick(&editor,Qt::Key_A,Qt::ShiftModifier);
    nvim.input("中文<Esc>"); QTRY_COMPARE(editor.text(),QString::fromUtf8("one two!中文"));
    QTest::keyClick(&editor,Qt::Key_U); QTRY_COMPARE(editor.text(),QString("one two!"));
    QSignalSpy saved(&nvim,&NvimInputHandler::saveRequested); nvim.input(":w<CR>");
    QTRY_COMPARE(saved.count(),1);
}
void TestNvimInputHandler::vimrc()
{
    QTemporaryDir config; QFile init(config.path()+"/init.vim"); QVERIFY(init.open(QIODevice::WriteOnly));
    init.write("nnoremap Q A!<Esc>\n"); init.close();
    QsciScintilla editor; editor.setEolMode(QsciScintilla::EolUnix); editor.setText("one");
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady()); nvim.input("Q"); QTRY_COMPARE(editor.text(),QString("one!"));
}
void TestNvimInputHandler::externalEdits()
{
    QTemporaryDir config; QsciScintilla editor; editor.setEolMode(QsciScintilla::EolUnix); editor.setText("old");
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady()); nvim.input("A!<Esc>"); QTRY_COMPARE(editor.text(),QString("old!"));
    editor.setText("new text"); editor.setCursorPosition(0,0);
    nvim.input("dw"); QTRY_COMPARE(editor.text(),QString("text"));
    nvim.stop(); QCOMPARE(editor.text(),QString("text"));
}
void TestNvimInputHandler::readonly()
{
    QTemporaryDir config; QsciScintilla editor; editor.setText("keep"); editor.setReadOnly(true);
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady()); nvim.input("dd"); QTest::qWait(250); QCOMPARE(editor.text(),QString("keep"));
}

void TestNvimInputHandler::scrolling()
{
    QTemporaryDir config; QsciScintilla editor; editor.resize(800,400); editor.show();
    QStringList lines; for(int i=0;i<200;++i) lines << QString("line %1").arg(i);
    editor.setText(lines.join('\n')); editor.setCursorPosition(0,0);
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady());
    auto line=[&editor]() { return editor.SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
        editor.SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)); };
    QTest::keyClick(&editor,Qt::Key_D,Qt::ControlModifier);
    QTRY_VERIFY(line()>0); const auto half=line();
    const auto page=editor.SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
    QVERIFY(half<page);
    QTest::keyClick(&editor,Qt::Key_U,Qt::ControlModifier); QTRY_COMPARE(line(),0L);
    QTest::keyClick(&editor,Qt::Key_F,Qt::ControlModifier); QTRY_VERIFY(line()>half);
    QTest::keyClick(&editor,Qt::Key_B,Qt::ControlModifier);
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE),0L);
    nvim.input("100Gzt"); QTRY_COMPARE(line(),99L);
    QTRY_COMPARE(editor.SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE),99L);
    nvim.input("L"); QTRY_VERIFY(line()>99L);
    nvim.input("H"); QTRY_COMPARE(line(),99L);
    nvim.input("M"); QTRY_VERIFY(line()>99L && line()<99+page);
}
void TestNvimInputHandler::saveBurst()
{
    QTemporaryDir config; QsciScintilla editor; editor.setEolMode(QsciScintilla::EolUnix); editor.setText("one");
    NvimInputHandler nvim(&editor); QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady()); QString saved;
    connect(&nvim,&NvimInputHandler::saveRequested,&editor,[&]() { saved=editor.text(); });
    nvim.input("A!<Esc>:w<CR>"); QTRY_COMPARE(saved,QString("one!"));
    nvim.input("A?<Esc>"); QTest::keyClick(&editor,Qt::Key_S,Qt::ControlModifier);
    QTRY_COMPARE(saved,QString("one!?"));
    QString executed; connect(&nvim,&NvimInputHandler::executeRequested,&editor,[&]() { executed=editor.text(); });
    nvim.input("A;<Esc>"); QTest::keyClick(&editor,Qt::Key_Return,Qt::ControlModifier);
    QTRY_COMPARE(executed,QString("one!?;"));
}

void TestNvimInputHandler::emptyEditor()
{
    QTemporaryDir config; QsciScintilla editor;
    editor.setEolMode(QsciScintilla::EolUnix);
    NvimInputHandler nvim(&editor);
    QVERIFY(nvim.start(NvimInputHandler::executablePath(),config.path()));
    QTRY_VERIFY(nvim.isReady());
    nvim.input("iSELECT 1;<Esc>");
    QTRY_COMPARE(editor.text(),QString("SELECT 1;"));
    nvim.input("u"); QTRY_COMPARE(editor.text(),QString(""));
    nvim.input("i中文<Esc>"); QTRY_COMPARE(editor.text(),QString::fromUtf8("中文"));
}
QTEST_MAIN(TestNvimInputHandler)
#include "TestNvimInputHandler.moc"
