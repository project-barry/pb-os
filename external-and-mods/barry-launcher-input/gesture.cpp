#include "gesture.h"

#include "touchrules.h"
#include "virtualmouse.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>

namespace barry {
namespace {

// The Launcher's touch rules (touchrules.h): how far a tap may wander, how
// long its click waits for a touch that drags, how long it may be down.
constexpr double TAP_SLOP = touch::SLOP;
constexpr double TAP_S = touch::TAP_S;
constexpr double TAP_MAX_S = touch::TAP_MAX_S;
// Two fingers never land at once: the pointer waits this long at the start
// of a touch, so a second finger makes a clean scroll, not a jump and then
// a scroll.
constexpr double SETTLE_S = 0.07;
constexpr double SCROLL_PX = touch::WHEEL_PX;  // design pixels per notch

}  // namespace

TrackpadGesture::TrackpadGesture(std::shared_ptr<MouseCore> mouse, Callbacks callbacks)
    : m_mouse(std::move(mouse)), m_cb(std::move(callbacks))
{
}

void TrackpadGesture::button(int code, bool down, bool up)
{
    if (down && m_cb.clicked)
        m_cb.clicked();
    m_mouse->button(code, down, up);
}

void TrackpadGesture::due(double now)
{
    if (m_clickAt && now >= *m_clickAt) {
        m_clickAt.reset();
        button(BTN_LEFT, true, true);
    }
}

void TrackpadGesture::reset()
{
    m_clickAt.reset();
    if (m_dragging)
        m_mouse->button(BTN_LEFT, false, true);
    for (int i = 0; i < 2; i++)
        if (m_held[i])
            m_mouse->button(i == 0 ? BTN_LEFT : BTN_RIGHT, false, true);
    m_contacts.clear();
    m_pad.clear();
    m_dragging = false;
    m_held[0] = m_held[1] = 0;
    if (m_cb.held)
        m_cb.held(0, 0);
    if (m_touching && m_cb.touching)
        m_cb.touching(false);
    m_touching = false;
}

void TrackpadGesture::frame(const std::map<int, Point> &live, double now, double stamp)
{
    for (auto it = m_contacts.begin(); it != m_contacts.end();) {
        if (live.count(it->first)) {
            ++it;
            continue;
        }
        const Contact c = it->second;
        const int id = it->first;
        it = m_contacts.erase(it);
        up(id, c, now);
    }
    std::vector<std::pair<double, double>> padMoves;
    for (const auto &[id, p] : live) {
        auto it = m_contacts.find(id);
        if (it == m_contacts.end()) {
            const Contact c{m_cb.region ? int(m_cb.region(p.x, p.y)) : int(Pad), p.x, p.y};
            m_contacts[id] = c;
            down(id, c, now);
        } else if (p.x != it->second.x || p.y != it->second.y) {
            if (std::find(m_pad.begin(), m_pad.end(), id) != m_pad.end())
                padMoves.emplace_back(p.x - it->second.x, p.y - it->second.y);
            it->second.x = p.x;
            it->second.y = p.y;
        }
    }
    move(padMoves, now, stamp);
    const bool touching = !m_pad.empty();
    if (touching != m_touching) {
        m_touching = touching;
        if (m_cb.touching)
            m_cb.touching(touching);
    }
}

void TrackpadGesture::down(int id, const Contact &c, double now)
{
    if (c.region == Left || c.region == Right) {
        const int i = c.region == Left ? 0 : 1;
        m_held[i]++;
        if (m_cb.held)
            m_cb.held(m_held[0], m_held[1]);
        if (m_cb.pressed)
            m_cb.pressed(i);
        button(i == 0 ? BTN_LEFT : BTN_RIGHT, true, false);
        return;
    }
    if (c.region != Pad)
        return;
    if (m_pad.empty()) {
        m_fingers = 0;
        m_moved = 0;
        m_started = now;
        m_last = 0;
        m_velocity = 0;
        m_heldX = m_heldY = 0;
        // A tap's click waits a moment: a touch right after it drags.
        if (m_clickAt) {
            m_clickAt.reset();
            m_dragging = true;
            m_dragMoved = false;
            button(BTN_LEFT, true, false);
        }
    }
    m_pad.push_back(id);
    m_fingers = std::max(m_fingers, int(m_pad.size()));
    if (m_fingers >= 2)
        m_heldX = m_heldY = 0;
}

void TrackpadGesture::move(const std::vector<std::pair<double, double>> &padMoves, double now, double stamp)
{
    if (padMoves.empty())
        return;
    const double dt = m_last ? (stamp - m_last) * 1000 : 0.0;
    m_last = stamp;
    double dx = 0, dy = 0;
    for (const auto &m : padMoves) {
        dx += m.first;
        dy += m.second;
    }
    dx /= double(padMoves.size());
    dy /= double(padMoves.size());
    const double dist = std::hypot(dx, dy);
    m_moved += dist;
    if (m_dragging && dist > 0)
        m_dragMoved = true;
    if (m_fingers >= 2 && !m_dragging) {
        // Two fingers scroll, the content following them.
        m_mouse->scroll(-dx / SCROLL_PX, -dy / SCROLL_PX);
        return;
    }
    // Pointer acceleration: pixels per millisecond of finger speed raise the
    // gain up to 3.5x. The speed is smoothed over a few frames, so the
    // panel's jitter under a resting finger is not sped up with it.
    if (dt > 0)
        m_velocity += 0.3 * (dist / std::max(4.0, dt) - m_velocity);
    const double speed = m_cb.speed ? m_cb.speed() : 1.6;
    const double gain = speed * (1 + 1.25 * std::min(2.0, m_velocity));
    if (!m_dragging && now - m_started < SETTLE_S) {
        m_heldX += dx * gain;
        m_heldY += dy * gain;
        return;
    }
    m_mouse->move(m_heldX + dx * gain, m_heldY + dy * gain);
    m_heldX = m_heldY = 0;
}

void TrackpadGesture::up(int id, const Contact &c, double now)
{
    if (c.region == Left || c.region == Right) {
        const int i = c.region == Left ? 0 : 1;
        m_held[i] = std::max(0, m_held[i] - 1);
        if (m_cb.held)
            m_cb.held(m_held[0], m_held[1]);
        m_mouse->button(i == 0 ? BTN_LEFT : BTN_RIGHT, false, true);
        return;
    }
    if (c.region == Dismiss) {
        if (m_cb.region && m_cb.region(c.x, c.y) == Dismiss && m_cb.dismissed)
            m_cb.dismissed();
        return;
    }
    auto it = std::find(m_pad.begin(), m_pad.end(), id);
    if (it == m_pad.end())
        return;
    m_pad.erase(it);
    if (!m_pad.empty())
        return;
    if (m_dragging) {
        m_dragging = false;
        m_mouse->button(BTN_LEFT, false, true);
        // Tap, tap: a double click.
        if (!m_dragMoved && now - m_started < TAP_S)
            button(BTN_LEFT, true, true);
        return;
    }
    if (m_moved > TAP_SLOP || now - m_started > TAP_MAX_S)
        return;
    if (m_fingers == 1)
        m_clickAt = now + TAP_S;
    else
        button(m_fingers == 2 ? BTN_RIGHT : BTN_MIDDLE, true, true);
}

}  // namespace barry
