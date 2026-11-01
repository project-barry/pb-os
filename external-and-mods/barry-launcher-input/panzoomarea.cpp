#include "panzoomarea.h"

#include "touchrules.h"

#include <QMouseEvent>
#include <QPointerEvent>
#include <QQuickWindow>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

double monotonic()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int MOUSE_ID = -100;  // a mouse stands in for one finger

// The Launcher's layout, for designScale().
constexpr double DESIGN_W = 1240, DESIGN_H = 1080;

bool isMouse(const QPointerEvent *event)
{
    return event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
           || event->type() == QEvent::MouseButtonRelease;
}

bool synthesized(const QPointerEvent *event)
{
    return isMouse(event) && static_cast<const QMouseEvent *>(event)->source() != Qt::MouseEventNotSynthesized;
}

int pointId(const QPointerEvent *event, const QEventPoint &point)
{
    return isMouse(event) ? MOUSE_ID : point.id();
}

}  // namespace

void barry::takeFromTaps(QPointerEvent *event, const QEventPoint &point, QObject *taker)
{
    // A passive grab cannot be cancelled as such: each tap handler watching
    // the touch is made its grabber, and taker takes the touch from it,
    // which cancels it (no tap as it lifts).
    const auto watchers = event->passiveGrabbers(point);
    for (const QPointer<QObject> &watcher : watchers) {
        if (!watcher || !watcher->inherits("QQuickTapHandler"))
            continue;
        event->setExclusiveGrabber(point, watcher);
        event->setExclusiveGrabber(point, taker);
        event->removePassiveGrabber(point, watcher);
    }
}

// --- GestureSurface -----------------------------------------------------

GestureSurface::GestureSurface(QQuickItem *parent)
    : QQuickItem(parent),
      m_gesture({
          [this](double x, double y) { tap(Single, x, y); },
          [this](double x, double y) { tap(Double, x, y); },
          [this](double x, double y) { tap(Long, x, y); },
          [this] { showView(); },
          nullptr,
      })
{
    setFiltersChildMouseEvents(true);
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_due.setSingleShot(true);
    connect(&m_due, &QTimer::timeout, this, [this] {
        m_gesture.due(monotonic());
        refresh();
    });
}

GestureSurface::~GestureSurface()
{
    setWindow(nullptr);
}

void GestureSurface::setInteractive(bool interactive)
{
    if (interactive == m_interactive)
        return;
    m_interactive = interactive;
    if (!interactive)
        cancel();
    emit interactiveChanged();
}

double GestureSurface::designScale() const
{
    if (!m_window || m_window->width() <= 0 || m_window->height() <= 0)
        return 1;
    return std::min(m_window->width() / DESIGN_W, m_window->height() / DESIGN_H);
}

void GestureSurface::setWindow(QQuickWindow *window)
{
    if (window == m_window)
        return;
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    if (m_window) {
        // Every move and lift of the touches it follows, wherever Qt
        // delivers them (a child holding the touch, or this).
        m_window->installEventFilter(this);
        connect(m_window, &QQuickWindow::afterAnimating, this, &GestureSurface::tick);
    }
}

void GestureSurface::refresh()
{
    const bool active = !m_live.empty();
    const bool dragging = m_gesture.dragging() && active;
    const bool moving = dragging || m_gesture.animating();
    if (!active) {
        setKeepTouchGrab(false);
        setKeepMouseGrab(false);
    }
    if (active != m_active) {
        m_active = active;
        emit activeChanged();
    }
    if (dragging != m_dragging) {
        m_dragging = dragging;
        emit draggingChanged();
    }
    if (moving != m_moving) {
        m_moving = moving;
        emit movingChanged();
    }
    arm();
}

void GestureSurface::arm()
{
    m_due.stop();
    if (const auto due = m_gesture.nextDue())
        m_due.start(std::max(0, int(std::ceil((*due - monotonic()) * 1000))));
}

void GestureSurface::kick()
{
    if (m_window)
        m_window->update();
}

void GestureSurface::tick()
{
    if (!m_gesture.animating())
        return;
    m_gesture.setScale(designScale());
    syncIn();
    const bool more = m_gesture.step(monotonic());
    refresh();
    if (more)
        kick();
}

void GestureSurface::stopGlide()
{
    m_gesture.stop();
    refresh();
}

void GestureSurface::cancel()
{
    m_gesture.cancel();
    m_live.clear();
    refresh();
}

bool GestureSurface::mine(QObject *grabber) const
{
    // This, an item inside it, or a handler on one.
    QObject *o = grabber;
    while (o && !qobject_cast<QQuickItem *>(o))
        o = o->parent();
    auto *item = qobject_cast<QQuickItem *>(o);
    return item && (item == this || isAncestorOf(item));
}

void GestureSurface::run(QPointerEvent *event)
{
    m_gesture.setScale(designScale());
    syncIn();
    const double stamp = event->timestamp() ? double(event->timestamp()) / 1000.0 : monotonic();
    m_gesture.frame(m_live, monotonic(), stamp);
    if (m_gesture.dragging() && !m_live.empty()) {
        // The touch is this one's now: taken from what is under it, unless
        // that keeps it (a slider, a handler at work), which lets it go.
        for (const QEventPoint &point : event->points()) {
            if (!m_live.count(pointId(event, point)))
                continue;
            QObject *grabber = event->exclusiveGrabber(point);
            if (grabber == this)
                continue;
            auto *item = qobject_cast<QQuickItem *>(grabber);
            if (grabber && (!item || item->keepTouchGrab() || item->keepMouseGrab())) {
                cancel();
                return;
            }
            barry::takeFromTaps(event, point, this);
            event->setExclusiveGrabber(point, this);
        }
        // And kept: a page swipe around does not take a list's scroll back.
        setKeepTouchGrab(true);
        setKeepMouseGrab(true);
    }
    refresh();
    if (m_gesture.animating())
        kick();
}

bool GestureSurface::press(QPointerEvent *event)
{
    if (!m_interactive || !isVisible() || !isEnabled())
        return false;
    bool added = false;
    for (const QEventPoint &point : event->points()) {
        if (point.state() != QEventPoint::Pressed)
            continue;
        const int id = pointId(event, point);
        if (m_live.count(id))
            continue;
        const QPointF at = mapFromScene(point.scenePosition());
        if (!QRectF(0, 0, width(), height()).contains(at))
            continue;
        m_live[id] = {at.x(), at.y()};
        added = true;
    }
    if (!added)
        return false;
    run(event);
    // A touch that caught a fling taps nothing.
    return m_gesture.dragging();
}

void GestureSurface::track(QPointerEvent *event)
{
    bool any = false;
    for (const QEventPoint &point : event->points()) {
        auto it = m_live.find(pointId(event, point));
        if (it == m_live.end())
            continue;
        // Taken by something around this (a page swipe): let go.
        QObject *grabber = event->exclusiveGrabber(point);
        if (grabber && !mine(grabber)) {
            cancel();
            return;
        }
        any = true;
        if (point.state() == QEventPoint::Released) {
            m_live.erase(it);
        } else {
            const QPointF at = mapFromScene(point.scenePosition());
            it->second = {at.x(), at.y()};
        }
    }
    if (any)
        run(event);
}

bool GestureSurface::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window || m_live.empty())
        return false;
    switch (event->type()) {
    case QEvent::TouchBegin:
        // Every finger down anew: none left from before.
        if (!m_live.count(MOUSE_ID))
            cancel();
        break;
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        auto *pe = static_cast<QPointerEvent *>(event);
        if (!synthesized(pe))
            track(pe);
        break;
    }
    case QEvent::TouchCancel:
        cancel();
        break;
    default:
        break;
    }
    return false;
}

bool GestureSurface::childMouseEventFilter(QQuickItem *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        auto *pe = static_cast<QPointerEvent *>(event);
        // Mouse events made from touches: the touch itself is handled.
        if (synthesized(pe))
            return m_dragging;
        // Presses are seen here (the window sees the rest): one this takes
        // goes no further.
        return press(pe);
    }
    default:
        return false;
    }
}

void GestureSurface::touchEvent(QTouchEvent *event)
{
    if (!m_interactive) {
        event->ignore();
        return;
    }
    if (event->type() == QEvent::TouchCancel) {
        cancel();
        return;
    }
    press(event);
    event->accept();
}

void GestureSurface::mousePressEvent(QMouseEvent *event)
{
    if (!m_interactive) {
        event->ignore();
        return;
    }
    press(event);
    event->accept();
}

void GestureSurface::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
}

void GestureSurface::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void GestureSurface::wheelEvent(QWheelEvent *event)
{
    m_gesture.setScale(designScale());
    syncIn();
    if (!m_interactive || !wheels()) {
        event->ignore();
        return;
    }
    double nx = event->angleDelta().x() / 120.0, ny = event->angleDelta().y() / 120.0;
    if (nx == 0 && ny == 0) {
        const double notch = barry::touch::WHEEL_PX * designScale();
        nx = event->pixelDelta().x() / notch;
        ny = event->pixelDelta().y() / notch;
    }
    m_gesture.wheel(event->position().x(), event->position().y(), nx, ny, monotonic());
    refresh();
    event->accept();
}

void GestureSurface::touchUngrabEvent()
{
    // Qt lets go of every touch as it ends; only one taken away while
    // fingers are still down is cancelled.
    if (!m_live.empty() && !m_live.count(MOUSE_ID))
        cancel();
}

void GestureSurface::mouseUngrabEvent()
{
    if (m_live.count(MOUSE_ID))
        cancel();
}

void GestureSurface::itemChange(ItemChange change, const ItemChangeData &value)
{
    switch (change) {
    case ItemSceneChange:
        cancel();
        setWindow(value.window);
        break;
    case ItemVisibleHasChanged:
    case ItemEnabledHasChanged:
        if (!value.boolValue) {
            cancel();
            stopGlide();
        }
        break;
    default:
        break;
    }
    QQuickItem::itemChange(change, value);
}

// --- PanZoomArea ----------------------------------------------------------

PanZoomArea::PanZoomArea(QQuickItem *parent) : GestureSurface(parent)
{
    limits();
}

void PanZoomArea::limits()
{
    m_gesture.setZoomEnabled(true);
    m_gesture.setAxes(true, true);
    m_gesture.setZoomRange(m_minZoom, m_maxZoom);
    if (m_bounds.isNull())
        m_gesture.clearBounds();
    else
        m_gesture.setBounds(m_bounds.left(), m_bounds.top(), m_bounds.right(), m_bounds.bottom());
    m_gesture.setDoubleTapZoom(m_doubleTapZoom);
}

void PanZoomArea::syncIn()
{
    m_gesture.setSize(width(), height());
    limits();
}

void PanZoomArea::showView()
{
    emit viewChanged();
}

void PanZoomArea::tap(Tap kind, double x, double y)
{
    switch (kind) {
    case Single:
        emit tapped(x, y);
        break;
    case Double:
        emit doubleTapped(x, y);
        break;
    case Long:
        emit longPressed(x, y);
        break;
    }
}

void PanZoomArea::set(barry::PanZoomGesture::View view)
{
    const auto before = m_gesture.view();
    m_gesture.setSize(width(), height());
    limits();
    m_gesture.setView(view);
    refresh();
    const auto after = m_gesture.view();
    if (after.x != before.x || after.y != before.y || after.zoom != before.zoom)
        emit viewChanged();
}

void PanZoomArea::setCenterX(qreal x)
{
    auto v = m_gesture.view();
    v.x = x;
    set(v);
}

void PanZoomArea::setCenterY(qreal y)
{
    auto v = m_gesture.view();
    v.y = y;
    set(v);
}

void PanZoomArea::setZoom(qreal zoom)
{
    auto v = m_gesture.view();
    v.zoom = zoom;
    set(v);
}

void PanZoomArea::setMinZoom(qreal zoom)
{
    if (zoom == m_minZoom)
        return;
    m_minZoom = zoom;
    emit zoomRangeChanged();
    set(m_gesture.view());
}

void PanZoomArea::setMaxZoom(qreal zoom)
{
    if (zoom == m_maxZoom)
        return;
    m_maxZoom = zoom;
    emit zoomRangeChanged();
    set(m_gesture.view());
}

void PanZoomArea::setBounds(const QRectF &bounds)
{
    if (bounds == m_bounds)
        return;
    m_bounds = bounds;
    emit boundsChanged();
    set(m_gesture.view());
}

void PanZoomArea::setDoubleTapZoom(bool on)
{
    if (on == m_doubleTapZoom)
        return;
    m_doubleTapZoom = on;
    m_gesture.setDoubleTapZoom(on);
    emit doubleTapZoomChanged();
}

QPointF PanZoomArea::mapToContent(qreal x, qreal y) const
{
    const auto v = m_gesture.view();
    return {(x - width() / 2) / v.zoom + v.x, (y - height() / 2) / v.zoom + v.y};
}

QPointF PanZoomArea::mapFromContent(qreal x, qreal y) const
{
    const auto v = m_gesture.view();
    return {(x - v.x) * v.zoom + width() / 2, (y - v.y) * v.zoom + height() / 2};
}

void PanZoomArea::zoomBy(qreal factor)
{
    zoomBy(factor, width() / 2, height() / 2);
}

void PanZoomArea::zoomBy(qreal factor, qreal x, qreal y)
{
    m_gesture.setScale(designScale());
    syncIn();
    m_gesture.zoomAround(x, y, factor, monotonic());
    refresh();
    kick();
}

// --- TouchScroll ----------------------------------------------------------

TouchScroll::TouchScroll(QQuickItem *parent) : GestureSurface(parent)
{
    m_gesture.setZoomEnabled(false);
    m_gesture.setZoomRange(1, 1);
    m_gesture.setAxes(false, false);
}

void TouchScroll::componentComplete()
{
    GestureSurface::componentComplete();
    if (m_flickable)
        return;
    const auto children = childItems();
    for (QQuickItem *child : children) {
        if (child->inherits("QQuickFlickable")) {
            setFlickable(child);
            break;
        }
    }
}

void TouchScroll::setFlickable(QQuickItem *flickable)
{
    if (flickable == m_flickable)
        return;
    stopGlide();
    m_flickable = flickable;
    m_shown = false;
    // It keeps its look; its touches are this one's.
    if (m_flickable)
        m_flickable->setProperty("interactive", false);
    emit flickableChanged();
}

void TouchScroll::syncIn()
{
    QQuickItem *f = m_flickable;
    if (!f) {
        m_axisX = m_axisY = false;
        m_gesture.setAxes(false, false);
        return;
    }
    const auto num = [f](const char *name) { return f->property(name).toReal(); };
    const double w = f->width(), h = f->height();
    const double cx = num("contentX"), cy = num("contentY");
    const double minX = num("originX") - num("leftMargin");
    const double minY = num("originY") - num("topMargin");
    const double maxX = std::max(minX, num("originX") + num("contentWidth") + num("rightMargin") - w);
    const double maxY = std::max(minY, num("originY") + num("contentHeight") + num("bottomMargin") - h);
    // Flickable's flickableDirection: 1 across, 2 down, 3 both; automatic
    // (0, 0xc): whichever way the content overflows.
    const int dir = f->property("flickableDirection").toInt() & 3;
    m_axisX = (dir == 0 || (dir & 1)) && maxX > minX + 0.5;
    m_axisY = (dir == 0 || (dir & 2)) && maxY > minY + 0.5;
    m_gesture.setSize(w, h);
    m_gesture.setAxes(m_axisX, m_axisY);
    m_gesture.setBounds(minX + w / 2, minY + h / 2, maxX + w / 2, maxY + h / 2);
    const barry::PanZoomGesture::View v{cx + w / 2, cy + h / 2, 1};
    // Moved from outside (positionViewAtEnd, a binding): a glide stops.
    if (m_shown && (std::abs(cx - m_shownX) > 0.5 || std::abs(cy - m_shownY) > 0.5)) {
        m_gesture.setView(v);
        m_shown = false;
    } else {
        m_gesture.syncView(v);
    }
}

void TouchScroll::showView()
{
    QQuickItem *f = m_flickable;
    if (!f)
        return;
    const auto v = m_gesture.view();
    if (m_axisX)
        f->setProperty("contentX", v.x - f->width() / 2);
    if (m_axisY)
        f->setProperty("contentY", v.y - f->height() / 2);
    m_shownX = f->property("contentX").toReal();
    m_shownY = f->property("contentY").toReal();
    m_shown = true;
}

bool TouchScroll::wheels() const
{
    return m_axisX || m_axisY;
}
