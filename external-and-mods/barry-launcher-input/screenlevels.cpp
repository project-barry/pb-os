#include "screenlevels.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace {
QString levelsPath()
{
    const QString env = qEnvironmentVariable("BARRY_SCREEN_LEVELS");
    return env.isEmpty() ? QStringLiteral("/var/lib/steamos-sm8550/thor-screen-brightness.json") : env;
}

// As pbos-qamd's get_state gives it.
int percent(const QJsonValue &v)
{
    return v.isDouble() ? static_cast<int>(std::nearbyint(qBound(0.0, v.toDouble(), 1.0) * 100)) : -1;
}
}  // namespace

ScreenLevels *ScreenLevels::create(QQmlEngine *, QJSEngine *)
{
    return new ScreenLevels;
}

ScreenLevels::ScreenLevels(QObject *parent) : QObject(parent), m_path(levelsPath())
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(20);
    connect(&m_settle, &QTimer::timeout, this, &ScreenLevels::read);
    // The file is always replaced (a rename), so its folder is watched. The
    // timer is not started over: a drag is followed, not only its end.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_settle.isActive())
            m_settle.start();
    });
    m_watcher.addPath(QFileInfo(m_path).absolutePath());
    read();
}

void ScreenLevels::read()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const int top = percent(o.value(QLatin1String("top"))), bottom = percent(o.value(QLatin1String("bottom")));
    if (top < 0 || bottom < 0 || (top == m_top && bottom == m_bottom))
        return;
    m_top = top;
    m_bottom = bottom;
    emit changed();
}
