#include "edgeswipe.h"

#include "touchrules.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <linux/input.h>
#include <map>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

const char TOUCH_NAME[] = "bottom_touchscreen";
// As TouchTrackpad: the panel is portrait and turned right; screen x is raw
// y, screen y runs along raw x, bottom to top. Design pixels (s = 1).
constexpr double SCREEN_W = 1240, SCREEN_H = 1080;
// The finger's last moments, for its speed at release (the touch rules').
constexpr double VELOCITY_WINDOW_S = barry::touch::VELOCITY_WINDOW_S;

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

}  // namespace

EdgeSwipe::EdgeSwipe(QObject *parent) : QObject(parent)
{
    connect(this, &EdgeSwipe::edgeChanged, this, [this] { m_edgeNow = m_edge; });
    connect(this, &EdgeSwipe::thresholdChanged, this, [this] { m_thresholdNow = m_threshold; });
    const QString event = touchDevice();
    if (event.isEmpty()) {
        qWarning() << "Barry.Input: no" << TOUCH_NAME << "- no side swipes over apps";
        return;
    }
    const int fd = open(QStringLiteral("/dev/input/%1").arg(event).toLocal8Bit().constData(),
                        O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qWarning().noquote() << "Barry.Input: cannot read" << event << ":" << strerror(errno)
                             << "- no side swipes over apps";
        return;
    }
    m_stopFd = eventfd(0, EFD_CLOEXEC);
    m_reader = std::thread(&EdgeSwipe::read, this, fd);
}

EdgeSwipe::~EdgeSwipe()
{
    if (m_stopFd >= 0) {
        const uint64_t one = 1;
        (void)!write(m_stopFd, &one, sizeof one);
    }
    if (m_reader.joinable())
        m_reader.join();
    if (m_stopFd >= 0)
        close(m_stopFd);
}

void EdgeSwipe::setArmed(bool armed)
{
    if (armed == m_armed)
        return;
    m_armed = armed;
    m_armedNow = armed;
    emit armedChanged();
}

void EdgeSwipe::postTouched(int side)
{
    QMetaObject::invokeMethod(this, [this, side] { emit edgeTouched(side); }, Qt::QueuedConnection);
}

void EdgeSwipe::postStarted(int side)
{
    QMetaObject::invokeMethod(this, [this, side] {
        m_side = side;
        m_active = true;
        m_progress = 0;
        emit activeChanged();
        emit progressChanged();
        emit started(side);
    }, Qt::QueuedConnection);
}

void EdgeSwipe::postProgress(double progress)
{
    QMetaObject::invokeMethod(this, [this, progress] {
        if (!m_active || progress == m_progress)
            return;
        m_progress = progress;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void EdgeSwipe::postReleased(int side, double progress, double velocity)
{
    QMetaObject::invokeMethod(this, [this, side, progress, velocity] {
        m_progress = progress;
        m_active = false;
        emit progressChanged();
        emit activeChanged();
        emit released(side, progress, velocity);
    }, Qt::QueuedConnection);
}

void EdgeSwipe::postCancelled()
{
    QMetaObject::invokeMethod(this, [this] {
        const bool was = m_active;
        m_active = false;
        if (was)
            emit activeChanged();
        emit cancelled();
    }, Qt::QueuedConnection);
}

void EdgeSwipe::read(int fd)
{
    // Per slot, as the kernel keeps them: a new contact repeats no
    // coordinate that has not changed.
    std::map<int, std::pair<int, int>> raw;  // slot -> raw x, y
    std::map<int, bool> down;               // slot -> touching
    int slot = 0;
    int lead = -1;          // the slot of the touch's first finger
    bool fresh = false;     // the lead finger came down since the last frame
    int side = 0;           // the edge it came down at (0: none, or let go)
    bool started = false;   // past the threshold: the page follows it
    double startX = 0, startY = 0;  // screen position where it came down
    std::deque<std::pair<double, double>> recent;  // (time, screen x)

    auto screenX = [&](int s) { return double(raw[s].second); };
    auto screenY = [&](int s) { return SCREEN_H - 1 - double(raw[s].first); };
    auto inward = [&](double x) { return side > 0 ? x - startX : startX - x; };
    auto endTouch = [&](bool cancel) {
        if (started) {
            if (cancel) {
                postCancelled();
            } else {
                double v = 0;
                if (recent.size() >= 2) {
                    const auto &a = recent.front(), &b = recent.back();
                    if (b.first > a.first)
                        v = (side > 0 ? b.second - a.second : a.second - b.second) / (b.first - a.first);
                }
                const double x = recent.empty() ? startX : recent.back().second;
                postReleased(side, std::clamp(inward(x) / SCREEN_W, 0.0, 1.0), v);
            }
        }
        side = 0;
        started = false;
        recent.clear();
    };

    pollfd fds[2] = {{fd, POLLIN, 0}, {m_stopFd, POLLIN, 0}};
    while (true) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents)
            break;
        input_event evs[64];
        const ssize_t n = ::read(fd, evs, sizeof evs);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            break;  // the touchscreen went away
        }
        for (ssize_t i = 0; i < n / ssize_t(sizeof(input_event)); ++i) {
            const input_event &ev = evs[i];
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_SLOT) {
                    slot = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0) {
                        const bool first = std::none_of(down.begin(), down.end(), [](auto &d) { return d.second; });
                        down[slot] = true;
                        if (first) {
                            lead = slot;
                            fresh = true;
                        } else if (side) {
                            endTouch(true);  // a second finger: no side swipe
                        }
                    } else {
                        down[slot] = false;
                        if (slot == lead) {
                            endTouch(false);
                            lead = -1;
                        }
                    }
                } else if (ev.code == ABS_MT_POSITION_X) {
                    raw[slot].first = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    raw[slot].second = ev.value;
                }
                continue;
            }
            if (ev.type != EV_SYN || ev.code != SYN_REPORT || lead < 0 || !down[lead])
                continue;
            const double x = screenX(lead), now = monotonic();
            if (fresh) {
                fresh = false;
                side = 0;
                if (!m_armedNow)
                    continue;
                const double edge = m_edgeNow;
                if (x <= edge)
                    side = 1;
                else if (x >= SCREEN_W - 1 - edge)
                    side = -1;
                if (side) {
                    startX = x;
                    startY = screenY(lead);
                    recent.clear();
                    recent.emplace_back(now, x);
                    postTouched(side);
                }
                continue;
            }
            if (!side)
                continue;
            recent.emplace_back(now, x);
            while (recent.size() > 2 && now - recent.front().first > VELOCITY_WINDOW_S)
                recent.pop_front();
            if (!started) {
                const double in = inward(x);
                if (in < -m_thresholdNow) {
                    side = 0;  // back off the screen: not a swipe in
                    continue;
                }
                if (in >= m_thresholdNow) {
                    // Mostly sideways: a scroll along the edge is no swipe in.
                    if (std::abs(screenY(lead) - startY) > in) {
                        side = 0;
                        continue;
                    }
                    started = true;
                    postStarted(side);
                }
                continue;
            }
            postProgress(std::clamp(inward(x) / SCREEN_W, 0.0, 1.0));
        }
    }
    close(fd);
}
