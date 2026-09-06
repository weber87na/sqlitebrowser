#ifndef NVIMINPUTHANDLER_H
#define NVIMINPUTHANDLER_H
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QHash>
#include <QVariant>
#include <functional>
class QsciScintilla;
class QKeyEvent;

class NvimInputHandler : public QObject
{
    Q_OBJECT
public:
    explicit NvimInputHandler(QsciScintilla* editor, QObject* parent = nullptr);
    ~NvimInputHandler() override;
    static QString executablePath();
    static QString configDirectory();
    bool start(const QString& executable = QString(), const QString& config = QString());
    void stop();
    bool isActive() const { return m_process.state() != QProcess::NotRunning; }
    bool isReady() const { return m_ready; }
    QString status() const { return m_status; }
    QString mode() const { return m_mode; }
    void input(const QString& keys);
    using Reply = std::function<void(const QVariant&, const QVariant&)>;
    void lua(const QString& code, const QVariantList& args = {}, Reply reply = {});
signals:
    void statusChanged();
    void failed(const QString& message);
    void saveRequested();
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    void request(const QString& method, const QVariantList& args, Reply reply = {});
    void receive();
    void notification(const QString& method, const QVariantList& args);
    void snapshot();
    void applySnapshot(const QVariantMap& data);
    void synchronize();
    void resize();
    QString keyNotation(QKeyEvent* key) const;
    int position(int line, int byteColumn) const;
    QsciScintilla* m_editor;
    QProcess m_process;
    QTimer m_poll;
    QByteArray m_received;
    QHash<qlonglong, Reply> m_replies;
    qlonglong m_nextId = 0;
    bool m_ready = false;
    bool m_snapshotPending = false;
    bool m_applying = false;
    bool m_stopping = false;
    int m_epoch = 0;
    QString m_text;
    QString m_eol = "\n";
    QString m_mode = "n";
    QString m_status;
    QString m_commandLine;
    QString m_pendingKeys;
    QString m_config;
    QString m_clipboard;
    QString m_clipboardKind = "v";
};
#endif
