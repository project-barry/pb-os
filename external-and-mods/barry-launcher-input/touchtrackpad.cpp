#include "touchtrackpad.h"

#include "gesture.h"
#include "haptics.h"
#include "virtualmouse.h"

#include <linux/input.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

const char TOUCH_NAME[] = "bottom_touchscreen";
// Touches in screen pixels: bottompanel.h (on the Thor, the 1240 x 1080
// TopInput.qml lays out at s = 1; its panel is portrait and turned right).
constexpr int OWED_RETRY_MS = 5000;
constexpr int START_RETRY_MS = 2000;
const char *const REGION_NAMES[] = {"pad", "left", "right", "dismiss"};

double monotonic()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QString touchDevice()
{
    const QStringList events = QDir(QStringLiteral("/sys/class/input")).entryList({QStringLiteral("event*")});
    for (const QString &event : events) {
        QFile f(QStringLiteral("/sys/class/input/%1/device/name").arg(event));
        if (f.open(QIODevice::ReadOnly) && f.readAll().trimmed() == TOUCH_NAME)
            return event;
    }
    return {};
}

// Tell KWin to take, or leave, the touchscreen's events.
bool kwinTouch(const QString &event, bool enabled)
{
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/InputDevice/") + event,
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    m << QStringLiteral("org.kde.KWin.InputDevice") << QStringLiteral("enabled")
      << QVariant::fromValue(QDBusVariant(enabled));
    return QDBusConnection::sessionBus().call(m, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
}

}  // namespace

TouchTrackpad::TouchTrackpad(QObject *parent) : QObject(parent)
{
    m_startRetry.setInterval(START_RETRY_MS);
    connect(&m_startRetry, &QTimer::timeout, this, [this] {
        if (m_active && !m_reading)
            start();
        else
            m_startRetry.stop();
    });
    m_owedRetry.setInterval(OWED_RETRY_MS);
    connect(&m_owedRetry, &QTimer::timeout, this, [this] {
        if (m_reading || m_owed.isEmpty()) {
            m_owedRetry.stop();
        } else if (kwinTouch(m_owed, true)) {
            qInfo().noquote() << "Barry.Input: KWin has" << m_owed << "back, late";
            m_owed.clear();
            m_owedRetry.stop();
        }
    });
}

TouchTrackpad::~TouchTrackpad()
{
    if (m_reading)
        stop("quitting");  // never leave KWin without the touchscreen
}

void TouchTrackpad::setMouse(VirtualMouse *mouse)
{
    if (mouse == m_mouse)
        return;
    if (m_mouse)
        disconnect(m_mouse, nullptr, this, nullptr);
    m_mouse = mouse;
    emit mouseChanged();
    // QML may set active before the mouse, or before its device is made:
    // start once both are there.
    if (m_mouse)
        connect(m_mouse, &VirtualMouse::readyChanged, this, [this] {
            if (m_reading)
                stop("its mouse changed");
            if (m_active)
                start();
        });
    // A reader keeps the mouse it started with; start over with this one.
    if (m_reading)
        stop("its mouse changed");
    if (m_active)
        start();
}

void TouchTrackpad::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    emit activeChanged();
    if (active && !m_reading) {
        start();
    } else if (!active) {
        m_startRetry.stop();
        if (m_reading)
            stop("the trackpad closed");
    }
}

qreal TouchTrackpad::speed() const
{
    std::lock_guard<std::mutex> hold(m_lock);
    return m_speed;
}

void TouchTrackpad::setSpeed(qreal speed)
{
    if (!(speed >= 0.2 && speed <= 8))
        return;
    {
        std::lock_guard<std::mutex> hold(m_lock);
        if (speed == m_speed)
            return;
        m_speed = speed;
    }
    emit speedChanged();
}

void TouchTrackpad::setRegions(const QVariantMap &regions)
{
    Rect parsed[RegionCount];
    for (int i = 0; i < RegionCount; i++) {
        const QVariantList r = regions.value(QLatin1String(REGION_NAMES[i])).toList();
        if (r.size() == 4)
            parsed[i] = {r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble(), true};
    }
    {
        std::lock_guard<std::mutex> hold(m_lock);
        std::copy(std::begin(parsed), std::end(parsed), std::begin(m_regions));
    }
    m_regionsMap = regions;
    emit regionsChanged();
}

QVariantMap TouchTrackpad::held() const
{
    return {{QStringLiteral("left"), m_held[0] > 0}, {QStringLiteral("right"), m_held[1] > 0}};
}

QVariantMap TouchTrackpad::presses() const
{
    return {{QStringLiteral("left"), m_presses[0]}, {QStringLiteral("right"), m_presses[1]}};
}

TouchTrackpad::Region TouchTrackpad::region(double x, double y)
{
    const double fx = x / m_panel.width, fy = y / m_panel.height;
    std::lock_guard<std::mutex> hold(m_lock);
    for (Region r : {Left, Right, Dismiss, Pad}) {
        const Rect &b = m_regions[r];
        if (b.set && b.x <= fx && fx < b.x + b.w && b.y <= fy && fy < b.y + b.h)
            return r;
    }
    return None;
}

void TouchTrackpad::start()
{
    if (!m_mouse || !m_mouse->core())
        return;  // again once it has one
    // Until it works, again every START_RETRY_MS; said once.
    const bool retrying = m_startRetry.isActive();
    m_startRetry.start();
    const QString event = touchDevice();
    if (event.isEmpty()) {
        if (!retrying)
            qWarning() << "Barry.Input: no" << TOUCH_NAME << "- the trackpad window keeps its own touches";
        return;
    }
    const int fd = open(QStringLiteral("/dev/input/%1").arg(event).toLocal8Bit().constData(),
                        O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (!retrying)
            qWarning().noquote() << "Barry.Input: cannot read" << event << ":" << strerror(errno);
        return;
    }
    startGuard(event);
    if (!kwinTouch(event, false)) {
        stopGuard();
        close(fd);
        if (!retrying)
            qWarning() << "Barry.Input: KWin would not let go of the touchscreen: the trackpad window keeps its touches";
        return;
    }
    m_startRetry.stop();
    m_event = event;
    m_owed.clear();
    m_owedRetry.stop();
    // Set before the reader starts, which then reads it alone.
    m_panel = barry::BottomPanel::fromEnv();
    m_panel.readAxes(fd);
    m_stopFd = eventfd(0, EFD_CLOEXEC);
    m_reader = std::thread(&TouchTrackpad::read, this, fd, m_mouse->core());
    m_reading = true;
    emit readingChanged();
    qInfo().noquote() << "Barry.Input: the trackpad reads" << event << "itself; KWin leaves it alone";
}

void TouchTrackpad::stop(const char *why)
{
    const uint64_t one = 1;
    (void)!write(m_stopFd, &one, sizeof one);
    if (m_reader.joinable())
        m_reader.join();
    close(m_stopFd);
    m_stopFd = -1;
    m_touching = false;
    m_held[0] = m_held[1] = 0;
    emit touchingChanged();
    emit heldChanged();
    // The guard would give it back too, but later: a trackpad opened again
    // meanwhile would lose it.
    stopGuard();
    if (kwinTouch(m_event, true)) {
        qInfo().noquote() << QStringLiteral("Barry.Input: KWin has the touchscreen back (%1)").arg(QLatin1String(why));
    } else {
        qWarning().noquote() << QStringLiteral("Barry.Input: could not give KWin the touchscreen back (%1);"
                                               " asking again every %2 s")
                                    .arg(QLatin1String(why))
                                    .arg(OWED_RETRY_MS / 1000);
        m_owed = m_event;
        m_owedRetry.start();
    }
    m_event.clear();
    m_reading = false;
    emit readingChanged();
}

void TouchTrackpad::startGuard(const QString &event)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return;
    // At EOF (this process gone, or the guard stopped), read returns and KWin
    // gets the touchscreen back.
    static const char script[] =
        "read -r _; exec gdbus call --session --dest org.kde.KWin"
        " --object-path \"/org/kde/KWin/InputDevice/$1\""
        " --method org.freedesktop.DBus.Properties.Set org.kde.KWin.InputDevice enabled '<true>'"
        " >/dev/null 2>&1";
    const QByteArray ev = event.toLocal8Bit();
    char *const argv[] = {const_cast<char *>("sh"), const_cast<char *>("-c"), const_cast<char *>(script),
                          const_cast<char *>("barry_touch_guard"), const_cast<char *>(ev.constData()),
                          nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[0], STDIN_FILENO);
    pid_t pid = -1;
    const int err = posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(p[0]);
    if (err != 0) {
        close(p[1]);
        qWarning().noquote() << "Barry.Input: no guard for the touchscreen:" << strerror(err);
        return;
    }
    m_guardPid = pid;
    m_guardFd = p[1];
}

void TouchTrackpad::stopGuard()
{
    if (m_guardPid > 0) {
        kill(m_guardPid, SIGKILL);
        waitpid(m_guardPid, nullptr, 0);
    }
    if (m_guardFd >= 0)
        close(m_guardFd);
    m_guardPid = -1;
    m_guardFd = -1;
}

void TouchTrackpad::postTouching(bool touching)
{
    QMetaObject::invokeMethod(this, [this, touching] {
        if (touching != m_touching && m_reading) {
            m_touching = touching;
            emit touchingChanged();
        }
    }, Qt::QueuedConnection);
}

void TouchTrackpad::postHeld(int left, int right)
{
    QMetaObject::invokeMethod(this, [this, left, right] {
        if (m_held[0] != left || m_held[1] != right) {
            m_held[0] = left;
            m_held[1] = right;
            emit heldChanged();
        }
    }, Qt::QueuedConnection);
}

void TouchTrackpad::postPress(int button)
{
    QMetaObject::invokeMethod(this, [this, button] {
        m_presses[button]++;
        emit pressesChanged();
    }, Qt::QueuedConnection);
}

void TouchTrackpad::postClick()
{
    QMetaObject::invokeMethod(this, [this] {
        Haptics::instance()->click();
        emit clicked();
    }, Qt::QueuedConnection);
}

void TouchTrackpad::postDismiss()
{
    QMetaObject::invokeMethod(this, [this] { emit dismissTapped(); }, Qt::QueuedConnection);
}

void TouchTrackpad::read(int fd, std::shared_ptr<barry::MouseCore> mouse)
{
    int slot = 0;
    std::map<int, std::pair<int, int>> raw;  // slot -> raw x, y as the kernel keeps them
    std::map<int, int> tracking;             // slot -> tracking id
    barry::TrackpadGesture::Callbacks cb;
    cb.clicked = [this] { postClick(); };
    cb.held = [this](int left, int right) { postHeld(left, right); };
    cb.pressed = [this](int button) { postPress(button); };
    cb.dismissed = [this] { postDismiss(); };
    cb.touching = [this](bool touching) { postTouching(touching); };
    cb.speed = [this] {
        std::lock_guard<std::mutex> hold(m_lock);
        return m_speed;
    };
    cb.region = [this](double x, double y) { return barry::TrackpadGesture::Region(region(x, y)); };
    barry::TrackpadGesture g(mouse, cb);
    input_event events[64];
    for (;;) {
        int timeout = -1;
        if (const auto due = g.nextDue())
            timeout = std::max(0, int(std::ceil((*due - monotonic()) * 1000)));
        pollfd fds[2] = {{fd, POLLIN, 0}, {m_stopFd, POLLIN, 0}};
        const int n = poll(fds, 2, timeout);
        if (n < 0 && errno != EINTR)
            break;
        if (fds[1].revents)
            break;
        g.due(monotonic());
        if (n <= 0 || !(fds[0].revents & POLLIN)) {
            if (fds[0].revents & (POLLERR | POLLHUP))
                break;  // the touchscreen went away
            continue;
        }
        const ssize_t got = ::read(fd, events, sizeof events);
        if (got <= 0)
            continue;
        for (size_t i = 0; i < size_t(got) / sizeof(input_event); i++) {
            const input_event &ev = events[i];
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_SLOT) {
                    slot = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0)
                        tracking[slot] = ev.value;
                    else
                        tracking.erase(slot);
                } else if (ev.code == ABS_MT_POSITION_X) {
                    raw[slot].first = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    raw[slot].second = ev.value;
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                // When the panel saw it: frames read together are still
                // milliseconds apart.
                const double stamp = double(ev.input_event_sec) + double(ev.input_event_usec) / 1e6;
                std::map<int, barry::TrackpadGesture::Point> live;
                for (const auto &[s, id] : tracking) {
                    const auto r = raw[s];
                    live[id] = {m_panel.x(r.first, r.second), m_panel.y(r.first, r.second)};
                }
                g.frame(live, monotonic(), stamp);
            }
        }
    }
    close(fd);
    g.reset();
    mouse->button(BTN_LEFT, false, true);
    mouse->button(BTN_RIGHT, false, true);
    mouse->button(BTN_MIDDLE, false, true);
}
