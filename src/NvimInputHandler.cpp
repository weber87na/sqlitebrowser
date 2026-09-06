#include "NvimInputHandler.h"
#include "NvimMessagePack.h"
#include <Qsci/qsciscintilla.h>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QStandardPaths>
#include <algorithm>

NvimInputHandler::NvimInputHandler(QsciScintilla* editor, QObject* parent) : QObject(parent), m_editor(editor)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &NvimInputHandler::receive);
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus) {
        m_poll.stop(); m_ready = false; m_snapshotPending = false; m_replies.clear();
        if(!m_stopping) {
            m_status = tr("Neovim stopped (%1); SQL text preserved").arg(code);
            emit statusChanged(); emit failed(m_status);
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if(error == QProcess::FailedToStart) emit failed(m_process.errorString());
    });
    m_poll.setInterval(40);
    connect(&m_poll, &QTimer::timeout, this, &NvimInputHandler::snapshot);
    connect(m_editor, &QsciScintilla::textChanged, this, [this]() {
        if(m_ready && !m_applying) synchronize();
    });
    // Installed after the legacy handler, so native input takes precedence.
    m_editor->installEventFilter(this);
    m_editor->viewport()->installEventFilter(this);
}
NvimInputHandler::~NvimInputHandler() { stop(); }
QString NvimInputHandler::executablePath()
{
    if(!qEnvironmentVariable("DB4S_NVIM").isEmpty()) return qEnvironmentVariable("DB4S_NVIM");
    const QString bundled = QCoreApplication::applicationDirPath() + "/nvim/bin/nvim.exe";
    if(QFileInfo::exists(bundled)) return bundled;
    return QStandardPaths::findExecutable("nvim");
}
QString NvimInputHandler::configDirectory()
{
    if(!qEnvironmentVariable("DB4S_NVIM_CONFIG").isEmpty()) return qEnvironmentVariable("DB4S_NVIM_CONFIG");
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/nvim";
}
bool NvimInputHandler::start(const QString& executable, const QString& config)
{
    if(isActive()) return true;
    const QString path = executable.isEmpty() ? executablePath() : executable;
    if(path.isEmpty()) return false;
    m_stopping = false; m_ready = false; m_received.clear(); m_replies.clear();
    m_pendingKeys.clear(); m_text.clear(); m_inputsPending=0; m_tick=-1; m_buffer=-1; m_snapshotPending = false;
    m_config = config.isEmpty() ? configDirectory() : config;
    QDir().mkpath(m_config);
    m_status = tr("Starting Neovim…"); emit statusChanged();
    m_process.start(path, {"--embed", "--headless", "-u", "NONE", "-i", "NONE", "-n"});
    if(!m_process.waitForStarted(1500)) return false;
    request("nvim_get_api_info", {}, [this](const QVariant& result, const QVariant& error) {
        if(error.isValid()) { emit failed(error.toString()); stop(); return; }
        const auto api = result.toList();
        const int channel = api.value(0).toInt();
        request("nvim_ui_attach", {80, 24, QVariantMap{{"rgb",true},{"ext_linegrid",true},
            {"ext_cmdline",true},{"ext_messages",true}}});
        QDir().mkpath(m_runtime.path()+"/autoload");
        QFile::copy(":/vim/repeat.vim",m_runtime.path()+"/autoload/repeat.vim");
        lua("vim.opt.runtimepath:prepend(...)", {m_runtime.path()});
        QFile plugin(":/vim/surround.vim"); plugin.open(QIODevice::ReadOnly);
        request("nvim_exec2", {QString::fromUtf8(plugin.readAll()), QVariantMap{}});
        QFile script(":/vim/bridge.lua"); script.open(QIODevice::ReadOnly);
        if(!script.isOpen()) { emit failed(tr("Neovim bridge resource missing")); stop(); return; }
        lua(QString::fromUtf8(script.readAll()), {channel, m_config}, [this](const QVariant&, const QVariant& err) {
            if(err.isValid()) { m_status=err.toList().value(1).toString(); emit failed(m_status); stop(); return; }
            m_ready = true;
            // Force the initial editor contents into the native buffer.
            m_text = QString(QChar(0));
            synchronize(); resize(); m_poll.start();
            const QString keys = m_pendingKeys; m_pendingKeys.clear();
            if(!keys.isEmpty()) input(keys);
            snapshot();
        });
    });
    return true;
}
void NvimInputHandler::stop()
{
    m_stopping = true; m_poll.stop(); m_ready = false;
    if(isActive()) { m_process.kill(); m_process.waitForFinished(1000); }
    m_replies.clear(); m_snapshotPending = false;
}
void NvimInputHandler::request(const QString& method, const QVariantList& args, Reply reply)
{
    if(!isActive()) return;
    const qlonglong id = ++m_nextId;
    if(reply) m_replies.insert(id, std::move(reply));
    m_process.write(NvimMessagePack::encode(QVariantList{0, id, method, args}));
}
void NvimInputHandler::lua(const QString& code, const QVariantList& args, Reply reply)
{ request("nvim_exec_lua", {code, args}, std::move(reply)); }
void NvimInputHandler::receive()
{
    m_received += m_process.readAllStandardOutput();
    while(!m_received.isEmpty()) {
        NvimMessagePack::Reader reader(m_received);
        const auto message = reader.value().toList();
        if(reader.invalid) { emit failed(tr("Invalid Neovim RPC stream")); stop(); return; }
        if(!reader.complete) return;
        m_received.remove(0, reader.pos);
        if(message.value(0).toInt() == 1 && message.size() == 4) {
            const auto callback = m_replies.take(message[1].toLongLong());
            if(callback) callback(message[3], message[2]);
        } else if(message.value(0).toInt() == 2 && message.size() == 3)
            notification(message[1].toString(), message[2].toList());
    }
}
void NvimInputHandler::notification(const QString& method, const QVariantList& args)
{
    if(method == "db4s_clipboard") {
        QStringList lines; for(const auto& line : args.value(0).toList()) lines << line.toString();
        QString value = lines.join('\n');
        if(args.value(1).toString() == "V") value += '\n';
        m_clipboard = value; m_clipboardKind = args.value(1).toString();
        QApplication::clipboard()->setText(value);
    } else if(method == "db4s_save") {
        // Flush native edits before Save reads the SQL widget, even when :w is
        // part of the same input burst as the change.
        flushAction(false);
    }
    else if(method == "db4s_error") { m_message = args.value(0).toString(); m_status = m_message; emit statusChanged(); }
    else if(method == "redraw") {
        for(const auto& item : args) {
            const auto event = item.toList(); const QString name = event.value(0).toString();
            for(int i = 1; i < event.size(); ++i) {
                const auto values = event[i].toList();
                if(name == "cmdline_show") {
                    m_commandLine = values.value(2).toString() + values.value(3).toString();
                    for(const auto& chunk : values.value(0).toList()) m_commandLine += chunk.toList().value(1).toString();
                } else if(name == "cmdline_hide") m_commandLine.clear();
                else if(name == "msg_show") {
                    QString message;
                    for(const auto& chunk : values.value(1).toList()) message += chunk.toList().value(1).toString();
                    if(!message.isEmpty()) { m_message = message; m_status = message; emit statusChanged(); }
                }
            }
        }
        if(!m_commandLine.isEmpty()) { m_status = m_commandLine; emit statusChanged(); }
    }
}
void NvimInputHandler::flushAction(bool execute)
{
    if(!m_ready) return;
    if(m_inputsPending > 0) {
        QTimer::singleShot(10,this,[this,execute]() { flushAction(execute); });
        return;
    }
    synchronize();
    lua("return _G.db4s_snapshot(-1,-1)", {}, [this,execute](const QVariant& value, const QVariant& error) {
        if(!error.isValid()) {
            applySnapshot(value.toMap());
            if(execute) emit executeRequested(); else emit saveRequested();
        }
    });
}
void NvimInputHandler::synchronize()
{
    if(!m_ready || m_applying) return;
    const QString text = m_editor->text();
    if(text != m_text) {
        ++m_epoch; m_text = text;
        m_eol = m_editor->eolMode() == QsciScintilla::EolWindows ? "\r\n" :
                m_editor->eolMode() == QsciScintilla::EolMac ? "\r" : "\n";
        QString normalized = text; normalized.replace("\r\n", "\n"); normalized.replace('\r','\n');
        const bool eol = normalized.endsWith('\n');
        if(eol) normalized.chop(1);
        QVariantList lines; for(const auto& line : normalized.split('\n')) lines << line;
        const int pos = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
        const int row = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,pos));
        m_lastCaret = pos;
        const int col = pos - int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,row));
        lua("local lines,eol,row,col,ro=...; vim.bo.modifiable=true; "
            "vim.api.nvim_buf_set_lines(0,0,-1,true,lines); vim.bo.endofline=eol; "
            "vim.bo.fixendofline=false; pcall(vim.api.nvim_win_set_cursor,0,{math.min(row+1,#lines),col}); "
            "vim.bo.readonly=ro; vim.bo.modifiable=not ro", {lines,eol,row,col,m_editor->isReadOnly()});
    }
    const int caret = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
    if(caret != m_lastCaret) {
        m_lastCaret = caret;
        const int row = int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,caret));
        lua("pcall(vim.api.nvim_win_set_cursor,0,{...})", {row+1,caret-position(row,0)});
    }
}
void NvimInputHandler::input(const QString& keys)
{
    if(!isActive()) return;
    if(!m_ready) { m_pendingKeys += keys; return; }
    synchronize();
    m_message.clear();
    QString clipboard = QApplication::clipboard()->text();
    const QString kind = clipboard == m_clipboard ? m_clipboardKind : "v";
    if(kind == "V" && clipboard.endsWith('\n')) clipboard.chop(1);
    const bool clipboardChanged = QApplication::clipboard()->text() != m_clipboard;
    if(clipboardChanged) { m_clipboard = QApplication::clipboard()->text(); m_clipboardKind = kind; }
    ++m_inputsPending;
    // nvim_input is a fast API: wait for deferred buffer/cursor updates before
    // enqueueing keys, otherwise they can operate on the previous document.
    lua("local text,kind,ro,changed=...; if changed then vim.g.db4s_clipboard=text; "
        "vim.g.db4s_clipboard_kind=kind end; vim.bo.readonly=ro; vim.bo.modifiable=not ro",
        {clipboard, kind, m_editor->isReadOnly(), clipboardChanged},
        [this,keys](const QVariant&,const QVariant& error) {
            if(error.isValid()) { --m_inputsPending; return; }
            request("nvim_input", {keys}, [this](const QVariant&,const QVariant&) {
                lua("return true", {}, [this](const QVariant&,const QVariant&) { --m_inputsPending; });
            });
        });
}
void NvimInputHandler::snapshot()
{
    if(!m_ready || m_snapshotPending || m_inputsPending > 0) return;
    synchronize();
    const int epoch = m_epoch;
    m_snapshotPending = true;
    lua("return _G.db4s_snapshot(...)", {m_tick,m_buffer}, [this,epoch](const QVariant& value, const QVariant& error) {
        m_snapshotPending = false;
        if(!error.isValid() && epoch == m_epoch) applySnapshot(value.toMap());
    });
}
int NvimInputHandler::position(int line, int byteColumn) const
{
    line = std::max(0, std::min(line, m_editor->lines()-1));
    const int start = int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,line));
    const int end = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION,line));
    return std::min(end, start + std::min(end-start, std::max(0, byteColumn)));
}
void NvimInputHandler::applySnapshot(const QVariantMap& data)
{
    if(data.isEmpty()) return;
    m_applying = true;
    m_tick = data.value("tick").toLongLong(); m_buffer = data.value("buffer").toInt();
    if(data.value("lines").type() == QVariant::List) {
        QStringList lines; for(const auto& line : data.value("lines").toList()) lines << line.toString();
        QString text = lines.join(m_eol);
        if(data.value("eol").toBool()) text += m_eol;
        if(text != m_editor->text()) {
            // Replace only the changed UTF-8 range so markers and scrolling survive.
            const QByteArray old = m_editor->text().toUtf8(), now = text.toUtf8();
            int first = 0;
            while(first < old.size() && first < now.size() && old[first] == now[first]) ++first;
            while(first > 0 && first < old.size() && (quint8(old[first]) & 0xc0) == 0x80) --first;
            int a = old.size(), b = now.size();
            while(a > first && b > first && old[a-1] == now[b-1]) { --a; --b; }
            while(a < old.size() && (quint8(old[a]) & 0xc0) == 0x80) { ++a; ++b; }
            const bool readOnly = m_editor->isReadOnly(); m_editor->setReadOnly(false);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,first);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,a);
            const QByteArray replacement = now.mid(first,b-first);
            m_editor->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET, replacement.size(), replacement.constData());
            m_editor->setReadOnly(readOnly);
        }
        m_text = text;
    }
    m_mode = data.value("mode").toString();
    const auto cursor = data.value("cursor").toList(), anchor = data.value("anchor").toList();
    const int caret = position(cursor.value(0).toInt()-1,cursor.value(1).toInt());
    const int start = position(anchor.value(0).toInt()-1,anchor.value(1).toInt());
    const bool visual = m_mode.startsWith('v') || m_mode.startsWith('V') || m_mode.startsWith(QChar(22));
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETSELECTIONMODE,
        m_mode.startsWith(QChar(22)) ? QsciScintillaBase::SC_SEL_RECTANGLE : QsciScintillaBase::SC_SEL_STREAM);
    if(visual) {
        int first = std::min(start,caret), last = std::max(start,caret);
        if(m_mode.startsWith('V')) {
            const int lo = std::min(anchor.value(0).toInt(),cursor.value(0).toInt())-1;
            const int hi = std::max(anchor.value(0).toInt(),cursor.value(0).toInt())-1;
            first = position(lo,0);
            last = hi + 1 < m_editor->lines() ? position(hi+1,0) : position(hi,2147483647);
        } else last = int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONAFTER,last));
        if(m_mode.startsWith(QChar(22))) {
            const bool right = cursor.value(1).toInt() >= anchor.value(1).toInt();
            const int a = right ? start : int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONAFTER,start));
            const int c = right ? int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONAFTER,caret)) : caret;
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETRECTANGULARSELECTIONANCHOR,a);
            m_editor->SendScintilla(QsciScintillaBase::SCI_SETRECTANGULARSELECTIONCARET,c);
        } else m_editor->SendScintilla(QsciScintillaBase::SCI_SETSEL,caret>=start ? first:last,caret>=start ? last:first);
    } else m_editor->SendScintilla(QsciScintillaBase::SCI_SETEMPTYSELECTION,caret);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETCARETSTYLE,
        m_mode.startsWith('i') || m_mode.startsWith('R') ? QsciScintillaBase::CARETSTYLE_LINE : QsciScintillaBase::CARETSTYLE_BLOCK);
    m_editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,std::max(0,data.value("top").toInt()-1));
    QString label = m_mode.startsWith('i') ? "INSERT" : m_mode.startsWith('R') ? "REPLACE" :
        m_mode.startsWith(QChar(22)) ? "VISUAL BLOCK" : m_mode.startsWith('V') ? "VISUAL LINE" :
        m_mode.startsWith('v') ? "VISUAL" : m_mode.startsWith('c') ? "COMMAND" : "NORMAL";
    const QString recording = data.value("recording").toString();
    if(!recording.isEmpty()) label += " | recording @" + recording;
    m_status = m_commandLine.isEmpty() ? "NVIM — " + label + (m_message.isEmpty() ? "" : " | " + m_message) : m_commandLine;
    m_lastCaret = int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
    m_applying = false; emit statusChanged();
}
void NvimInputHandler::resize()
{
    if(!m_ready) return;
    const QFontMetrics metrics(m_editor->font());
    const int columns = std::max(20,m_editor->viewport()->width()/std::max(1,metrics.horizontalAdvance('M')));
    const int rows = std::max(3,int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN)));
    request("nvim_ui_try_resize", {columns,rows});
}
QString NvimInputHandler::keyNotation(QKeyEvent* key) const
{
    QString name;
    switch(key->key()) {
    case Qt::Key_Escape: name="Esc"; break;
    case Qt::Key_Return: case Qt::Key_Enter: name="CR"; break;
    case Qt::Key_Backspace: name="BS"; break;
    case Qt::Key_Delete: name="Del"; break;
    case Qt::Key_Tab: name="Tab"; break;
    case Qt::Key_Backtab: return "<S-Tab>";
    case Qt::Key_Left: name="Left"; break;
    case Qt::Key_Right: name="Right"; break;
    case Qt::Key_Up: name="Up"; break;
    case Qt::Key_Down: name="Down"; break;
    case Qt::Key_Home: name="Home"; break;
    case Qt::Key_End: name="End"; break;
    case Qt::Key_PageUp: name="PageUp"; break;
    case Qt::Key_PageDown: name="PageDown"; break;
    default: break;
    }
    const auto mods = key->modifiers();
    // AltGr produces text, not a Ctrl+Alt Vim command.
    if(mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::AltModifier) && !key->text().isEmpty())
        return QString(key->text()).replace("<","<LT>");
    if(name.isEmpty() && mods.testFlag(Qt::ControlModifier) && key->key() >= 32 && key->key() <= 126)
        name = QChar(key->key()).toLower();
    if(!name.isEmpty()) {
        if(mods.testFlag(Qt::ShiftModifier)) name.prepend("S-");
        if(mods.testFlag(Qt::AltModifier)) name.prepend("A-");
        if(mods.testFlag(Qt::ControlModifier)) name.prepend("C-");
        return "<"+name+">";
    }
    QString text = key->text();
    if(mods.testFlag(Qt::ShiftModifier) && key->key() >= Qt::Key_A && key->key() <= Qt::Key_Z)
        text = QChar(key->key());
    return text.replace("<","<LT>");
}
bool NvimInputHandler::eventFilter(QObject* object, QEvent* event)
{
    if((object != m_editor && object != m_editor->viewport()) || !isActive()) return false;
    if(event->type() == QEvent::Resize) { QTimer::singleShot(0,this,&NvimInputHandler::resize); return false; }
    if(event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Flush pending native input before the application reads SQL text.
        if(key->modifiers().testFlag(Qt::ControlModifier) &&
            (key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter || key->key()==Qt::Key_S)) {
            if(event->type()==QEvent::ShortcutOverride) key->accept();
            else flushAction(key->key()!=Qt::Key_S);
            return true;
        }
        const QString notation = keyNotation(key);
        if(notation.isEmpty()) return false;
        if(event->type()==QEvent::ShortcutOverride) { key->accept(); return true; }
        input(notation); return true;
    }
    if(event->type()==QEvent::InputMethod) {
        const auto* ime=static_cast<QInputMethodEvent*>(event);
        if(!ime->commitString().isEmpty()) { input(QString(ime->commitString()).replace("<","<LT>")); return true; }
    }
    if(event->type()==QEvent::MouseButtonRelease && m_ready) {
        QTimer::singleShot(0,this,[this]() {
            if(!m_ready) return;
            const int p=int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
            const int line=int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,p));
            const int col=p-position(line,0);
            const int a=int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART));
            const int end=int(m_editor->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND));
            if(end>a) {
                const int b=int(m_editor->SendScintilla(QsciScintillaBase::SCI_POSITIONBEFORE,end));
                const int al=int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,a));
                const int bl=int(m_editor->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,b));
                lua("local al,ac,bl,bc=...; vim.cmd('normal! '..string.char(27)); "
                    "vim.api.nvim_win_set_cursor(0,{al,ac}); vim.cmd('normal! v'); "
                    "vim.api.nvim_win_set_cursor(0,{bl,bc})",{al+1,a-position(al,0),bl+1,b-position(bl,0)});
            } else {
                lua("vim.cmd('normal! '..string.char(27)); pcall(vim.api.nvim_win_set_cursor,0,{...})",{line+1,col});
            }
            m_lastCaret = p;
        });
    }
    return false;
}
