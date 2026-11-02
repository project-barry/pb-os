// ShellState: what barry_launcher_shelld says, as it changes. shelld pushes
// its state over a local socket ($XDG_RUNTIME_DIR/barry_launcher/shelld.sock;
// SHELLD-EVENTS.md): one JSON line per topic as it connects (the snapshot),
// then a line for a topic only when it changes. Each payload is what the
// same HTTP GET answers (home: GET /home, ...), so the QML reads it as it
// read those.
//
// One connection a process: the QML's ShellState and KeyboardState share it
// (instance()). shelld not up yet, or restarting: it tries again, waiting a
// little longer each time (the only timer); connected again, the snapshot
// brings everything up to date. An older shelld, without the socket: it asks
// over HTTP instead, at the old intervals, for the topics someone watches
// (watch()), until the socket shows up.
//
// A tool's window (the Trackpad app, the Deck controls) says so (tool):
// going home, shelld then asks it to slide away (the tools topic names this
// process), leaving turns true, its QML plays that and calls left(), and
// shelld ends it as Barry comes forward. An older shelld never asks: it
// ends the tool at once, as before.
#pragma once

#include <QHash>
#include <QJsonValue>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QNetworkAccessManager;
class QQmlEngine;
class QJSEngine;

class ShellState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // The payloads: null until shelld has said.
    Q_PROPERTY(QJsonValue home READ home NOTIFY homeChanged)
    Q_PROPERTY(QJsonValue keyboard READ keyboard NOTIFY keyboardChanged)
    Q_PROPERTY(QJsonValue topKeyboard READ topKeyboard NOTIFY topKeyboardChanged)
    Q_PROPERTY(QJsonValue keyd READ keyd NOTIFY keydChanged)
    Q_PROPERTY(QJsonValue apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QJsonValue tools READ tools NOTIFY toolsChanged)
    // New versions of Project Barry's apps on offer (GET /updates;
    // shell/AppUpdates.qml).
    Q_PROPERTY(QJsonValue updates READ updates NOTIFY updatesChanged)
    // This process is a tool's window that slides away when asked.
    Q_PROPERTY(bool tool READ tool WRITE setTool NOTIFY toolChanged)
    // shelld asks this tool to go (Barry opens): it slides away, then calls
    // left(). Once true it stays so: the process is on its way out.
    Q_PROPERTY(bool leaving READ leaving NOTIFY leavingChanged)
    // On shelld's socket (else asking over HTTP, or nothing up).
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    // The process's one connection.
    static ShellState *instance();
    static ShellState *create(QQmlEngine *, QJSEngine *);

    QJsonValue home() const { return m_values.value(Home); }
    QJsonValue keyboard() const { return m_values.value(Keyboard); }
    QJsonValue topKeyboard() const { return m_values.value(TopKeyboard); }
    QJsonValue keyd() const { return m_values.value(Keyd); }
    QJsonValue apps() const { return m_values.value(Apps); }
    QJsonValue tools() const { return m_values.value(Tools); }
    QJsonValue updates() const { return m_values.value(Updates); }
    bool tool() const { return m_tool; }
    void setTool(bool tool);
    bool leaving() const { return m_leaving; }
    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    // The bottom keyboard's height (window pixels): shelld pushes the apps
    // up by it.
    Q_INVOKABLE void setKeyboardHeight(int height);
    // A topic this process follows ("home", "keyboard", "top-keyboard",
    // "keyd", "apps", "tools", "updates"): only those are asked over HTTP without the
    // socket.
    // Counted: each watch() wants an unwatch().
    Q_INVOKABLE void watch(const QString &topic);
    Q_INVOKABLE void unwatch(const QString &topic);
    // Ask for a topic now (after a change made over HTTP). On the socket
    // nothing to do: shelld pushes the change itself.
    Q_INVOKABLE void refresh(const QString &topic);
    // The tool has slid away: shelld brings Barry forward and ends this
    // process. Should it not within LEFT_QUIT_MS (shelld gone, or restarting),
    // the process quits by itself; at once without shelld's socket.
    Q_INVOKABLE void left();

signals:
    void homeChanged();
    void keyboardChanged();
    void topKeyboardChanged();
    void keydChanged();
    void appsChanged();
    void toolsChanged();
    void updatesChanged();
    void toolChanged();
    void leavingChanged();
    void connectedChanged();

private:
    enum Topic { Home, Keyboard, TopKeyboard, Keyd, Apps, Tools, Updates, TopicCount };

    ShellState();
    void connectNow();
    void retryLater();
    void readLines();
    void take(int topic, const QJsonValue &value);
    void sendHeight();
    void sendTool();
    void send(const QJsonObject &message);
    void takeTools();
    void updatePolling();
    void poll(int topic);
    static int topicOf(const QString &name);

    QLocalSocket m_socket;
    QString m_path;
    QTimer m_retry;
    int m_backoff = 0;
    bool m_failed = false;  // the last try found no socket: ask over HTTP

    QHash<int, QJsonValue> m_values;
    int m_watchers[TopicCount] = {};
    int m_height = -1;  // -1: none set
    bool m_tool = false;
    bool m_leaving = false;
    QTimer m_quit;  // left(): the process's own way out, should shelld not end it

    // Over HTTP (an older shelld).
    QNetworkAccessManager *m_net = nullptr;
    QTimer m_polls[TopicCount];
    bool m_asking[TopicCount] = {};
};
