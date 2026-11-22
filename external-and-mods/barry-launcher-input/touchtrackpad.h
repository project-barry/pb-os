// TouchTrackpad: Desktop Mode's trackpad read straight from the bottom
// touchscreen. Its gestures are TrackpadGesture's (gesture.h), as the Trackpad
// app's TrackpadArea's.
//
// Under KWin, touches on the bottom screen would reach the Trackpad window
// through KWin, and KWin's window move takes every touch while it runs: a
// finger lifted from the pad ends a move started with Left click held, and a
// new one never reaches the pad. So while active, KWin is told to leave the
// bottom touchscreen alone and this reads it on a thread of its own, moving
// mouse: the same gestures as TopInput.qml (move, tap to click, tap then
// touch to drag, two fingers to scroll, two- and three-finger taps for right
// and middle clicks, the Left and Right click buttons, the dismiss button).
// The window says where its pad and buttons are (regions, fractions of the
// screen) and draws what this reports (touching, held, presses).
//
// Not started (no touchscreen yet, KWin not letting go): tried again every
// few seconds while active. KWin gets the touchscreen back when this stops. Should the process die
// first, a small guard process (sh, waiting on a pipe from this one) gives
// it back; should KWin refuse, it is asked again every few seconds.
#pragma once

#include "bottompanel.h"
#include "virtualmouse.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <mutex>
#include <thread>


class TouchTrackpad : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(VirtualMouse *mouse READ mouse WRITE setMouse NOTIFY mouseChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    // {"pad"|"left"|"right"|"dismiss": [x, y, w, h]} as fractions of the screen
    Q_PROPERTY(QVariantMap regions READ regions WRITE setRegions NOTIFY regionsChanged)
    // Whether the touchscreen is this one's (KWin let go of it).
    Q_PROPERTY(bool reading READ reading NOTIFY readingChanged)
    Q_PROPERTY(bool touching READ touching NOTIFY touchingChanged)
    // The Left and Right click buttons: {"left": bool, "right": bool} held,
    // and {"left": n, "right": n} presses so far, so even a quick click can
    // light one up.
    Q_PROPERTY(QVariantMap held READ held NOTIFY heldChanged)
    Q_PROPERTY(QVariantMap presses READ presses NOTIFY pressesChanged)

public:
    explicit TouchTrackpad(QObject *parent = nullptr);
    ~TouchTrackpad() override;

    VirtualMouse *mouse() const { return m_mouse; }
    void setMouse(VirtualMouse *mouse);
    bool active() const { return m_active; }
    void setActive(bool active);
    qreal speed() const;
    void setSpeed(qreal speed);
    QVariantMap regions() const { return m_regionsMap; }
    void setRegions(const QVariantMap &regions);
    bool reading() const { return m_reading; }
    bool touching() const { return m_touching; }
    QVariantMap held() const;
    QVariantMap presses() const;

signals:
    void mouseChanged();
    void activeChanged();
    void speedChanged();
    void regionsChanged();
    void readingChanged();
    void touchingChanged();
    void heldChanged();
    void pressesChanged();
    // A tap on the dismiss button.
    void dismissTapped();
    // A mouse button went down (a tap's click, a drag, a click button): for
    // the window's haptics.
    void clicked();

private:
    struct Rect {
        double x = 0, y = 0, w = 0, h = 0;
        bool set = false;
    };
    enum Region { None = -1, Pad, Left, Right, Dismiss, RegionCount };

    void start();
    void stop(const char *why);
    void read(int fd, std::shared_ptr<barry::MouseCore> mouse);
    Region region(double x, double y);
    void startGuard(const QString &event);
    void stopGuard();
    void giveBack();

    // From the reader thread to the window's.
    void postTouching(bool touching);
    void postHeld(int left, int right);
    void postPress(int button);
    void postDismiss();
    void postClick();

    QPointer<VirtualMouse> m_mouse;
    bool m_active = false;
    bool m_reading = false;
    bool m_touching = false;
    int m_held[2] = {0, 0};
    int m_presses[2] = {0, 0};
    QVariantMap m_regionsMap;

    mutable std::mutex m_lock;  // m_regions, m_speed: the reader reads them
    Rect m_regions[RegionCount];
    double m_speed = 2.0;

    barry::BottomPanel m_panel;  // where its touches are on screen (set as it starts)
    QString m_event;    // the touchscreen's event node (event3), while reading
    QString m_owed;     // a touchscreen KWin has not taken back yet
    QTimer m_owedRetry;
    QTimer m_startRetry;
    int m_stopFd = -1;  // eventfd: the reader stops
    std::thread m_reader;
    int m_guardPid = -1;
    int m_guardFd = -1;
};
