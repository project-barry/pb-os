// VolumeLevels: the two screens' volume (0-100), for the quick settings, as
// they move: the top one's from $XDG_RUNTIME_DIR/pbos/volume.json (kept by
// barry-audio: Steam's slider, the volume keys), the bottom screen's apps'
// from ~/.config/barry_launcher/quick.json (statsd, barry-audio when linked).
// Both are watched (inotify through QFileSystemWatcher), nothing is polled.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class VolumeLevels : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // -1: not known (no such file).
    Q_PROPERTY(int top READ top NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    Q_PROPERTY(int bottom READ bottom NOTIFY changed)

public:
    static VolumeLevels *create(QQmlEngine *, QJSEngine *);

    int top() const { return m_top; }
    bool muted() const { return m_muted; }
    int bottom() const { return m_bottom; }

signals:
    void changed();

private:
    explicit VolumeLevels(QObject *parent = nullptr);
    void read();

    QString m_topPath, m_bottomPath;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;  // one read for a burst of events
    int m_top = -1, m_bottom = -1;
    bool m_muted = false;
};
