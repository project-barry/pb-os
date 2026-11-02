#include "shellstate.h"

#include <QCoreApplication>
#include <QDir>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <unistd.h>

namespace {
const char SHELLD[] = "http://127.0.0.1:47824";  // barry_launcher_shelld's HTTP API

// The topics' names on the socket, their HTTP paths and (without the
// socket) how often each was asked before.
struct TopicInfo {
    const char *name;
    const char *path;
    int pollMs;
};
constexpr TopicInfo TOPICS[] = {
    {"home", "/home", 50},
    {"keyboard", "/keyboard", 100},
    {"top-keyboard", "/top-keyboard", 100},
    {"keyd", "/keyd", 100},
    {"apps", "/apps", 1000},
    {"tools", "/tools", 1000},  // no one watches it: an older shelld has no /tools
    {"updates", "/updates", 5000},  // nor this: an older shelld has no /updates
};

// Waiting to try the socket again: from the first, doubling up to the last.
constexpr int RETRY_FIRST_MS = 100;
constexpr int RETRY_MAX_MS = 5000;
// A line that long without its end is not shelld's: dropped.
constexpr qint64 MAX_LINE = 4 * 1024 * 1024;
// A tool that has slid away waits this long at most for shelld to end it.
constexpr int LEFT_QUIT_MS = 1000;

QString socketPath()
{
    QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty())
        dir = QStringLiteral("/run/user/%1").arg(getuid());
    return QDir(dir).filePath(QStringLiteral("barry_launcher/shelld.sock"));
}
}  // namespace

ShellState *ShellState::instance()
{
    static ShellState *shared = new ShellState;
    return shared;
}

ShellState *ShellState::create(QQmlEngine *, QJSEngine *)
{
    ShellState *state = instance();
    QJSEngine::setObjectOwnership(state, QJSEngine::CppOwnership);
    return state;
}

ShellState::ShellState() : m_path(socketPath())
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &ShellState::connectNow);
    m_quit.setSingleShot(true);
    connect(&m_quit, &QTimer::timeout, qApp, &QCoreApplication::quit);
    for (int t = 0; t < TopicCount; t++) {
        m_polls[t].setInterval(TOPICS[t].pollMs);
        connect(&m_polls[t], &QTimer::timeout, this, [this, t] { poll(t); });
    }

    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_backoff = 0;
        m_failed = false;
        updatePolling();
        sendHeight();
        sendTool();
        emit connectedChanged();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        // shelld restarting: back soon, so not straight to HTTP.
        emit connectedChanged();
        retryLater();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        // shelld went away: disconnected above tries again.
        if (error == QLocalSocket::PeerClosedError || m_socket.state() == QLocalSocket::ConnectedState)
            return;
        // No socket (an older shelld, or none up yet): HTTP meanwhile.
        m_failed = true;
        updatePolling();
        retryLater();
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &ShellState::readLines);
    connectNow();
}

int ShellState::topicOf(const QString &name)
{
    for (int t = 0; t < TopicCount; t++)
        if (name == QLatin1String(TOPICS[t].name))
            return t;
    return -1;
}

void ShellState::connectNow()
{
    if (m_socket.state() == QLocalSocket::UnconnectedState)
        m_socket.connectToServer(m_path);
}

void ShellState::retryLater()
{
    if (m_retry.isActive())
        return;
    m_backoff = m_backoff ? qMin(m_backoff * 2, RETRY_MAX_MS) : RETRY_FIRST_MS;
    m_retry.start(m_backoff);
}

void ShellState::readLines()
{
    while (m_socket.canReadLine()) {
        const QByteArray line = m_socket.readLine();
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        const int t = topicOf(o.value(QLatin1String("topic")).toString());
        if (t >= 0)
            take(t, o.value(QLatin1String("data")));
    }
    if (m_socket.bytesAvailable() > MAX_LINE) {
        qWarning("ShellState: dropping an over-long line from shelld");
        m_socket.readAll();
    }
}

void ShellState::take(int topic, const QJsonValue &value)
{
    // Again as it was (the snapshot after a reconnect): no change to tell.
    const auto it = m_values.constFind(topic);
    if (it != m_values.constEnd() && it.value() == value)
        return;
    m_values.insert(topic, value);
    switch (topic) {
    case Home: emit homeChanged(); break;
    case Keyboard: emit keyboardChanged(); break;
    case TopKeyboard: emit topKeyboardChanged(); break;
    case Keyd: emit keydChanged(); break;
    case Apps: emit appsChanged(); break;
    case Tools:
        emit toolsChanged();
        takeTools();
        break;
    case Updates: emit updatesChanged(); break;
    }
}

void ShellState::takeTools()
{
    // Asked by pid: a tool started after an ask (the snapshot on connect
    // may still say it) is not the one asked.
    if (!m_tool || m_leaving)
        return;
    const QJsonObject leaving = tools().toObject().value(QLatin1String("leaving")).toObject();
    const qint64 pid = QCoreApplication::applicationPid();
    for (auto it = leaving.constBegin(); it != leaving.constEnd(); ++it) {
        if (it.value().toInteger(-1) == pid) {
            m_leaving = true;
            emit leavingChanged();
            return;
        }
    }
}

void ShellState::setTool(bool tool)
{
    if (tool == m_tool)
        return;
    m_tool = tool;
    sendTool();
    emit toolChanged();
    takeTools();
}

void ShellState::sendTool()
{
    if (m_tool && connected())
        send(QJsonObject{{QStringLiteral("tool"), true}});
}

void ShellState::left()
{
    if (!connected()) {
        QCoreApplication::quit();  // no one to end it
        return;
    }
    send(QJsonObject{{QStringLiteral("tool_left"), true}});
    if (!m_quit.isActive())
        m_quit.start(LEFT_QUIT_MS);
}

void ShellState::send(const QJsonObject &message)
{
    m_socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void ShellState::setKeyboardHeight(int height)
{
    if (height == m_height)
        return;
    m_height = height;
    sendHeight();
}

void ShellState::sendHeight()
{
    if (m_height < 0 || !connected())
        return;
    send(QJsonObject{{QStringLiteral("keyboard_height"), m_height}});
}

void ShellState::watch(const QString &topic)
{
    const int t = topicOf(topic);
    if (t < 0) {
        qWarning("ShellState: no topic %s", qPrintable(topic));
        return;
    }
    m_watchers[t]++;
    updatePolling();
}

void ShellState::unwatch(const QString &topic)
{
    const int t = topicOf(topic);
    if (t < 0 || m_watchers[t] == 0)
        return;
    m_watchers[t]--;
    updatePolling();
}

void ShellState::refresh(const QString &topic)
{
    const int t = topicOf(topic);
    if (t >= 0 && !connected() && m_failed)
        poll(t);
}

void ShellState::updatePolling()
{
    const bool http = m_failed && !connected();
    for (int t = 0; t < TopicCount; t++) {
        const bool on = http && m_watchers[t] > 0;
        if (on == m_polls[t].isActive())
            continue;
        if (on) {
            m_polls[t].start();
            poll(t);  // at once, as the old timers did
        } else {
            m_polls[t].stop();
        }
    }
}

void ShellState::poll(int topic)
{
    if (m_asking[topic])
        return;
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    m_asking[topic] = true;
    QString url = QLatin1String(SHELLD) + QLatin1String(TOPICS[topic].path);
    if (topic == Keyboard && m_height >= 0)
        url += QStringLiteral("?h=%1").arg(m_height);
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(2000);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, topic] {
        reply->deleteLater();
        m_asking[topic] = false;
        // On the socket by now: it has the newer word.
        if (reply->error() != QNetworkReply::NoError || connected())
            return;
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error == QJsonParseError::NoError)
            take(topic, doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
    });
}
