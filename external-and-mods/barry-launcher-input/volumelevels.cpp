#include "volumelevels.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {
QJsonObject object(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

int percent(const QJsonValue &v)
{
    return v.isDouble() ? qBound(0, static_cast<int>(v.toDouble()), 100) : -1;
}
}  // namespace

VolumeLevels *VolumeLevels::create(QQmlEngine *, QJSEngine *)
{
    return new VolumeLevels;
}

VolumeLevels::VolumeLevels(QObject *parent)
    : QObject(parent),
      m_topPath(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QLatin1String("/pbos/volume.json")),
      m_bottomPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1String("/barry_launcher/quick.json"))
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(20);
    connect(&m_settle, &QTimer::timeout, this, &VolumeLevels::read);
    // As ScreenLevels: the files are always replaced, a drag is followed.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_settle.isActive())
            m_settle.start();
    });
    for (const QString &path : {m_topPath, m_bottomPath}) {
        const QString dir = QFileInfo(path).absolutePath();
        QDir().mkpath(dir);
        m_watcher.addPath(dir);
    }
    read();
}

void VolumeLevels::read()
{
    const QJsonObject t = object(m_topPath);
    const int top = percent(t.value(QLatin1String("top"))), bottom = percent(object(m_bottomPath).value(QLatin1String("appsVolume")));
    const bool muted = t.value(QLatin1String("muted")).toBool();
    if (top == m_top && bottom == m_bottom && muted == m_muted)
        return;
    m_top = top;
    m_bottom = bottom;
    m_muted = muted;
    emit changed();
}
