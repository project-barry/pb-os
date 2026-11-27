// ScreenLevels: the two screens' brightness (their sliders, 0-100), for the
// quick settings. sm8550-thor-backlightd keeps both in a file that Steam's
// slider, the PB-OS tab and Barry change. This watches that file (inotify
// through QFileSystemWatcher, nothing polled), so a drag of Steam's slider
// shows on Barry's as it goes.
//
// The file is /var/lib/steamos-sm8550/thor-screen-brightness.json, or
// BARRY_SCREEN_LEVELS (tests): {"top": 0-1, "bottom": 0-1, ...}
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class ScreenLevels : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // -1: not known (no such file).
    Q_PROPERTY(int top READ top NOTIFY changed)
    Q_PROPERTY(int bottom READ bottom NOTIFY changed)

public:
    static ScreenLevels *create(QQmlEngine *, QJSEngine *);

    int top() const { return m_top; }
    int bottom() const { return m_bottom; }

signals:
    void changed();

private:
    explicit ScreenLevels(QObject *parent = nullptr);
    void read();

    QString m_path;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;  // one read for a burst of events
    int m_top = -1, m_bottom = -1;
};
