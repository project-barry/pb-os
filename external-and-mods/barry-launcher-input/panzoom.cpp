#include "panzoom.h"

#include "touchrules.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace barry {
namespace {

// Android's scroller (OverScroller's spline fling), its constants as they
// are there: a throw's distance and duration from its speed, and the curve
// it slows along.
constexpr double GRAVITY = 9.80665;  // m/s^2
constexpr double INCH_M = 39.37;     // inches per meter
constexpr double INFLEXION = 0.35;
constexpr double START_TENSION = 0.5;
constexpr double END_TENSION = 1.0;
const double DECELERATION_RATE = std::log(0.78) / std::log(0.9);
constexpr int SPLINE_SAMPLES = 100;

const std::array<double, SPLINE_SAMPLES + 1> &splinePosition()
{
    static const std::array<double, SPLINE_SAMPLES + 1> table = [] {
        std::array<double, SPLINE_SAMPLES + 1> t{};
        const double p1 = START_TENSION * INFLEXION;
        const double p2 = 1.0 - END_TENSION * (1.0 - INFLEXION);
        double xMin = 0;
        for (int i = 0; i < SPLINE_SAMPLES; i++) {
            const double alpha = double(i) / SPLINE_SAMPLES;
            double xMax = 1, x = 0, coef = 0;
            for (int n = 0; n < 64; n++) {
                x = xMin + (xMax - xMin) / 2;
                coef = 3 * x * (1 - x);
                const double tx = coef * ((1 - x) * p1 + x * p2) + x * x * x;
                if (std::abs(tx - alpha) < 1e-5)
                    break;
                if (tx > alpha)
                    xMax = x;
                else
                    xMin = x;
            }
            t[i] = coef * ((1 - x) * START_TENSION + x) + x * x * x;
        }
        t[SPLINE_SAMPLES] = 1;
        return t;
    }();
    return table;
}

// How far along its distance a throw is, t (0 to 1) along its duration.
double spline(double t)
{
    const auto &table = splinePosition();
    t = std::clamp(t, 0.0, 1.0);
    const int i = std::min(SPLINE_SAMPLES - 1, int(SPLINE_SAMPLES * t));
    const double t0 = double(i) / SPLINE_SAMPLES, t1 = double(i + 1) / SPLINE_SAMPLES;
    return table[i] + (t - t0) / (t1 - t0) * (table[i + 1] - table[i]);
}

struct Spread {
    double x = 0, y = 0, span = 0;
    int n = 0;
};

// The fingers' midpoint and spread (their mean distance from it), over the
// ids in both maps, at the positions in `at`.
Spread spread(const std::map<int, PanZoomGesture::Point> &at, const std::map<int, PanZoomGesture::Point> &other)
{
    Spread s;
    for (const auto &[id, p] : at) {
        if (!other.count(id))
            continue;
        s.x += p.x;
        s.y += p.y;
        s.n++;
    }
    if (!s.n)
        return s;
    s.x /= s.n;
    s.y /= s.n;
    for (const auto &[id, p] : at)
        if (other.count(id))
            s.span += std::hypot(p.x - s.x, p.y - s.y);
    s.span /= s.n;
    return s;
}

}  // namespace

PanZoomGesture::PanZoomGesture(Callbacks callbacks) : m_cb(std::move(callbacks)) {}

double PanZoomGesture::slop() const
{
    return touch::SLOP * m_scale;
}

void PanZoomGesture::setSize(double width, double height)
{
    m_w = width;
    m_h = height;
}

void PanZoomGesture::setScale(double scale)
{
    m_scale = scale > 0 ? scale : 1;
}

void PanZoomGesture::setZoomRange(double min, double max)
{
    m_minZoom = std::max(1e-6, min);
    m_maxZoom = std::max(m_minZoom, max);
    clamp(m_view);
}

void PanZoomGesture::setZoomEnabled(bool enabled)
{
    m_zoomOn = enabled;
}

void PanZoomGesture::setAxes(bool x, bool y)
{
    m_axisX = x;
    m_axisY = y;
}

void PanZoomGesture::setBounds(double minX, double minY, double maxX, double maxY)
{
    m_bounded = true;
    m_bx0 = minX;
    m_by0 = minY;
    m_bx1 = maxX;
    m_by1 = maxY;
}

void PanZoomGesture::clearBounds()
{
    m_bounded = false;
}

void PanZoomGesture::setDoubleTapZoom(bool on)
{
    m_doubleTap = on;
    if (!on)
        flushTap();
}

void PanZoomGesture::clamp(View &v) const
{
    if (m_zoomOn)
        v.zoom = std::clamp(v.zoom, m_minZoom, m_maxZoom);
    if (!m_bounded)
        return;
    v.x = m_bx1 < m_bx0 ? (m_bx0 + m_bx1) / 2 : std::clamp(v.x, m_bx0, m_bx1);
    v.y = m_by1 < m_by0 ? (m_by0 + m_by1) / 2 : std::clamp(v.y, m_by0, m_by1);
}

void PanZoomGesture::setView(const View &view)
{
    stop();
    syncView(view);
}

void PanZoomGesture::syncView(const View &view)
{
    m_view = view;
    if (!(m_view.zoom > 0))
        m_view.zoom = 1;
    clamp(m_view);
}

void PanZoomGesture::emitChanged()
{
    if (m_cb.changed)
        m_cb.changed();
}

void PanZoomGesture::stop()
{
    m_anim = None;
}

void PanZoomGesture::apply(double fx0, double fy0, double fx1, double fy1, double ratio)
{
    // A list moves along its axes only; the content under the old midpoint
    // goes to the new one, at the new zoom.
    if (!m_axisX)
        fx1 = fx0;
    if (!m_axisY)
        fy1 = fy0;
    View v = m_view;
    const double z1 = m_zoomOn ? std::clamp(v.zoom * ratio, m_minZoom, m_maxZoom) : v.zoom;
    const double px = v.x + (fx0 - m_w / 2) / v.zoom;
    const double py = v.y + (fy0 - m_h / 2) / v.zoom;
    v.x = px - (fx1 - m_w / 2) / z1;
    v.y = py - (fy1 - m_h / 2) / z1;
    v.zoom = z1;
    clamp(v);
    m_panX += fx1 - fx0;
    m_panY += fy1 - fy0;
    if (v.x == m_view.x && v.y == m_view.y && v.zoom == m_view.zoom)
        return;
    m_view = v;
    emitChanged();
}

void PanZoomGesture::startDrag()
{
    m_state = Dragging;
    m_longAt.reset();
    // A tap that waited for a second one: this touch is no second tap.
    if (m_secondTap)
        flushTap();
    m_secondTap = false;
    m_samples.clear();
    if (m_cb.dragStarted)
        m_cb.dragStarted();
}

void PanZoomGesture::frame(const std::map<int, Point> &live, double now, double stamp)
{
    if (live.empty() && m_live.empty())
        return;
    if (m_live.empty()) {
        // A new touch. One on a gliding view stops it, and drags from here.
        const bool caught = m_anim == Fling;
        stop();
        m_state = Pending;
        m_maxFingers = 0;
        m_downAt = now;
        m_travelX = m_travelY = m_spread = 0;
        m_flingArmed = true;
        m_samples.clear();
        m_panX = m_panY = 0;
        m_secondTap = m_tapAt && now <= *m_tapAt;
        m_longAt = now + touch::LONG_PRESS_S;
        if (caught && (m_axisX || m_axisY))
            startDrag();
    }

    const Spread was = spread(m_live, live);
    const Spread is = spread(live, m_live);
    const bool changed = live.size() != m_live.size() || size_t(is.n) != live.size();
    if (is.n) {
        const double ratio = is.n >= 2 && was.span > 0 && m_zoomOn ? is.span / was.span : 1;
        if (m_state == Pending) {
            m_travelX += is.x - was.x;
            m_travelY += is.y - was.y;
            if (is.n >= 2)
                m_spread += is.span - was.span;
            const double along = std::hypot(m_axisX ? m_travelX : 0, m_axisY ? m_travelY : 0);
            if (along > slop() || (m_zoomOn && (m_axisX || m_axisY) && std::abs(m_spread) > slop()))
                startDrag();  // and moves from the next frame: no jump by the slop
            else if (std::hypot(m_travelX, m_travelY) > slop())
                m_state = Spent;  // moved where nothing moves: no tap either
        } else if (m_state == Dragging) {
            apply(was.x, was.y, is.x, is.y, ratio);
        }
    }
    m_maxFingers = std::max(m_maxFingers, int(live.size()));
    if (m_maxFingers > 1) {
        m_longAt.reset();
        m_secondTap = false;
    }

    if (m_state == Dragging && !live.empty()) {
        if (changed) {
            // The midpoint jumps when a finger lands or lifts: the speed is
            // measured afresh from here. The finger left after a pinch
            // throws only once it moved by itself.
            m_samples.clear();
            if (m_live.size() >= 2 && live.size() == 1) {
                m_flingArmed = false;
                m_soloX = live.begin()->second.x;
                m_soloY = live.begin()->second.y;
            }
        } else if (!m_flingArmed && live.size() == 1) {
            const Point &p = live.begin()->second;
            if (std::hypot(p.x - m_soloX, p.y - m_soloY) > slop()) {
                m_flingArmed = true;
                m_samples.clear();
            }
        }
        if (!live.empty()) {
            m_samples.push_back({stamp, m_panX, m_panY, m_view.zoom});
            while (m_samples.size() > 2 && stamp - m_samples.front().t > touch::VELOCITY_WINDOW_S * 2)
                m_samples.pop_front();
        }
    }

    if (!live.empty()) {
        const Spread mid = spread(live, live);
        m_lastX = mid.x;
        m_lastY = mid.y;
    }
    m_live = live;
    if (live.empty())
        lift(now, stamp);
}

bool PanZoomGesture::velocity(double stamp, double &vx, double &vy) const
{
    // Least squares over the window; nothing if the finger rested.
    vx = vy = 0;
    if (m_samples.size() < 2 || stamp - m_samples.back().t > touch::STOPPED_S)
        return false;
    double n = 0, st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    double zMin = m_samples.back().zoom, zMax = zMin;
    const double t0 = m_samples.back().t;
    for (const Sample &s : m_samples) {
        if (t0 - s.t > touch::VELOCITY_WINDOW_S)
            continue;
        const double t = s.t - t0;
        n++;
        st += t;
        sx += s.x;
        sy += s.y;
        stt += t * t;
        stx += t * s.x;
        sty += t * s.y;
        zMin = std::min(zMin, s.zoom);
        zMax = std::max(zMax, s.zoom);
    }
    const double d = n * stt - st * st;
    if (n < 2 || d <= 0)
        return false;
    // Still zooming as it lifted: a pinch, not a throw.
    if (zMax / zMin > 1 + touch::PINCH_STILL)
        return false;
    vx = (n * stx - st * sx) / d;
    vy = (n * sty - st * sy) / d;
    return true;
}

void PanZoomGesture::lift(double now, double stamp)
{
    m_longAt.reset();
    const State state = m_state;
    m_state = Idle;
    if (state == Pending && m_maxFingers == 1 && now - m_downAt < touch::LONG_PRESS_S) {
        tap(m_lastX, m_lastY, now);
        return;
    }
    if (state == Pending || state == Spent) {
        if (m_secondTap)
            flushTap();
        m_secondTap = false;
        return;
    }
    double vx = 0, vy = 0;
    if (state == Dragging && m_flingArmed && velocity(stamp, vx, vy))
        startFling(vx, vy, now);
}

void PanZoomGesture::startFling(double vx, double vy, double now)
{
    if (!m_axisX)
        vx = 0;
    if (!m_axisY)
        vy = 0;
    double speed = std::hypot(vx, vy);
    if (speed < touch::FLING_MIN * m_scale)
        return;
    const double cap = touch::FLING_MAX * m_scale;
    if (speed > cap) {
        vx *= cap / speed;
        vy *= cap / speed;
        speed = cap;
    }
    const double coeff = GRAVITY * INCH_M * touch::DESIGN_PPI * m_scale * 0.84;
    const double l = std::log(INFLEXION * speed / (touch::FLING_FRICTION * coeff));
    m_animDur = std::exp(l / (DECELERATION_RATE - 1));
    const double dist = touch::FLING_FRICTION * coeff * std::exp(DECELERATION_RATE / (DECELERATION_RATE - 1) * l);
    // The content goes with the finger: the view's center the other way.
    m_dirX = -vx / speed;
    m_dirY = -vy / speed;
    m_flingDist = dist / m_view.zoom;
    m_from = m_view;
    m_animStart = now;
    m_anim = Fling;
}

void PanZoomGesture::tap(double x, double y, double now)
{
    if (!m_doubleTap) {
        if (m_cb.tapped)
            m_cb.tapped(x, y);
        return;
    }
    if (m_secondTap && m_tapAt
        && std::hypot(x - m_tapX, y - m_tapY) <= touch::DOUBLE_TAP_SLOP * m_scale) {
        m_tapAt.reset();
        m_secondTap = false;
        if (m_cb.doubleTapped)
            m_cb.doubleTapped(x, y);
        if (m_zoomOn)
            zoomAround(x, y, touch::DOUBLE_TAP_ZOOM, now);
        return;
    }
    flushTap();
    m_secondTap = false;
    m_tapAt = now + touch::TAP_S;
    m_tapX = x;
    m_tapY = y;
}

void PanZoomGesture::flushTap()
{
    if (!m_tapAt)
        return;
    m_tapAt.reset();
    if (m_cb.tapped)
        m_cb.tapped(m_tapX, m_tapY);
}

void PanZoomGesture::cancel()
{
    m_live.clear();
    m_state = Idle;
    m_longAt.reset();
    m_samples.clear();
    m_secondTap = false;
}

std::optional<double> PanZoomGesture::nextDue() const
{
    std::optional<double> at;
    if (m_longAt && m_state == Pending && m_maxFingers == 1)
        at = m_longAt;
    // A tap waits while its second one is down.
    if (m_tapAt && !(active() && m_secondTap))
        at = at ? std::min(*at, *m_tapAt) : *m_tapAt;
    return at;
}

void PanZoomGesture::due(double now)
{
    if (m_longAt && now >= *m_longAt && m_state == Pending && m_maxFingers == 1) {
        m_longAt.reset();
        m_state = Spent;
        if (m_secondTap)
            flushTap();
        m_secondTap = false;
        if (m_cb.longPressed)
            m_cb.longPressed(m_lastX, m_lastY);
    }
    if (m_tapAt && now >= *m_tapAt && !(active() && m_secondTap))
        flushTap();
}

void PanZoomGesture::wheel(double x, double y, double notchesX, double notchesY, double now)
{
    (void)now;
    stop();
    if (m_zoomOn) {
        if (notchesY != 0)
            apply(x, y, x, y, std::pow(touch::WHEEL_ZOOM, notchesY));
        return;
    }
    // Up (away) shows what is above: the view's center goes up.
    View v = m_view;
    if (m_axisX)
        v.x -= notchesX * touch::WHEEL_PX * m_scale / v.zoom;
    if (m_axisY)
        v.y -= notchesY * touch::WHEEL_PX * m_scale / v.zoom;
    clamp(v);
    if (v.x == m_view.x && v.y == m_view.y)
        return;
    m_view = v;
    emitChanged();
}

void PanZoomGesture::zoomAround(double x, double y, double factor, double now)
{
    stop();
    if (!m_zoomOn || !(factor > 0))
        return;
    m_from = m_view;
    m_zoomTo = std::clamp(m_view.zoom * factor, m_minZoom, m_maxZoom);
    m_zoomFx = x;
    m_zoomFy = y;
    m_animStart = now;
    m_animDur = touch::ZOOM_ANIM_S;
    m_anim = Zoom;
}

bool PanZoomGesture::step(double now)
{
    if (m_anim == None)
        return false;
    const double t = m_animDur > 0 ? std::clamp((now - m_animStart) / m_animDur, 0.0, 1.0) : 1.0;
    View v = m_from;
    bool blocked = false;
    if (m_anim == Fling) {
        const double d = m_flingDist * spline(t);
        View free{m_from.x + m_dirX * d, m_from.y + m_dirY * d, m_from.zoom};
        v = free;
        clamp(v);
        // Stopped at the bounds on every axis it was going along: over.
        const bool xStuck = m_dirX == 0 || v.x != free.x;
        const bool yStuck = m_dirY == 0 || v.y != free.y;
        blocked = xStuck && yStuck;
    } else {
        // Eased out: most of the way early, as Android's zoom steps.
        const double e = 1 - (1 - t) * (1 - t);
        const double z = m_from.zoom * std::pow(m_zoomTo / m_from.zoom, e);
        const double px = m_from.x + (m_zoomFx - m_w / 2) / m_from.zoom;
        const double py = m_from.y + (m_zoomFy - m_h / 2) / m_from.zoom;
        v = View{px - (m_zoomFx - m_w / 2) / z, py - (m_zoomFy - m_h / 2) / z, z};
        clamp(v);
    }
    const bool moved = v.x != m_view.x || v.y != m_view.y || v.zoom != m_view.zoom;
    m_view = v;
    if (t >= 1 || blocked)
        m_anim = None;
    if (moved)
        emitChanged();
    return m_anim != None;
}

}  // namespace barry
