#include "pageswipe.h"

#include "panzoomarea.h"

#include <QMouseEvent>
#include <QPointerEvent>
#include <QTouchEvent>
#include <QtMath>

namespace {
// A swipe is mostly sideways: its horizontal travel at least this many
// times its vertical travel when it starts.
constexpr qreal kSideways = 1.5;
// Thrown at least this fast (page widths per second), it goes that way
// whatever the distance.
constexpr qreal kFling = 0.6;
// After a swipe, touches this soon are no taps: the page is still moving
// (it settles in 380-460 ms), and a thumb meeting it was not aimed.
constexpr qint64 kSettleQuietMs = 350;
}  // namespace

PageSwipe::PageSwipe(QQuickItem *parent) : QQuickItem(parent)
{
    setFiltersChildMouseEvents(true);
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_clock.start();
}

void PageSwipe::setProgress(qreal progress)
{
    if (qFuzzyCompare(progress + 1, m_progress + 1))
        return;
    m_progress = progress;
    emit progressChanged();
}

void PageSwipe::setDragging(bool dragging)
{
    if (dragging == m_dragging)
        return;
    m_dragging = dragging;
    emit draggingChanged();
}

void PageSwipe::setPage(int page)
{
    if (page == m_page)
        return;
    m_page = page;
    emit pageChanged();
}

int PageSwipe::clampPage(int page) const
{
    return qBound(m_minPage, page, qMax(m_minPage, m_maxPage));
}

void PageSwipe::settleTo(int target)
{
    target = clampPage(target);
    setPage(target);
    emit settle(target, m_velocity);
}

void PageSwipe::goTo(int page)
{
    // A finger mid-swipe lets go of the page, and taps nothing either.
    if (m_pointId >= 0)
        m_spent.insert(m_pointId);
    m_pointId = -1;
    m_touches.clear();
    setDragging(false);
    m_velocity = 0;
    settleTo(page);
}

void PageSwipe::holdTaps(int ms)
{
    m_holdUntil = m_clock.elapsed() + ms;
}

void PageSwipe::holdTapsBelow(qreal x, qreal y, qreal radius, int ms)
{
    m_heldUntil = m_clock.elapsed() + ms;
    m_heldAt = QPointF(x, y);
    m_heldRadius = radius;
    m_heldPage = m_page;
}

bool PageSwipe::held(const QPointF &pos) const
{
    return m_clock.elapsed() < m_heldUntil && m_page == m_heldPage && qRound(m_progress) == m_heldPage
           && qAbs(pos.x() - m_heldAt.x()) <= m_heldRadius
           && pos.y() >= m_heldAt.y() - m_heldRadius / 2;
}

void PageSwipe::track(Touch &touch, const QPointF &pos)
{
    const qreal w = qMax<qreal>(1, width());
    const qint64 now = m_clock.elapsed();
    if (now > touch.lastMs) {
        const qreal v = (pos.x() - touch.last.x()) / w * 1000.0 / qreal(now - touch.lastMs);
        touch.velocity = 0.6 * v + 0.4 * touch.velocity;
        touch.lastMs = now;
    }
    touch.last = pos;
}

bool PageSwipe::take(QPointerEvent *event, const QEventPoint &point, Touch &touch)
{
    const QPointF pos = point.scenePosition();
    track(touch, pos);
    const qreal dx = pos.x() - touch.start.x();
    const qreal dy = pos.y() - touch.start.y();
    if (qAbs(dx) < m_threshold || qAbs(dx) < kSideways * qAbs(dy))
        return false;
    // Only a direction with a page to go to; and something that keeps its
    // touch (a slider) keeps it. Either way this touch is not a swipe,
    // whatever it does next.
    const int from = clampPage(qRound(touch.startProgress));
    const bool toward = dx > 0 ? from < m_maxPage : from > m_minPage;
    auto *grabber = qobject_cast<QQuickItem *>(event->exclusiveGrabber(point));
    if (!toward || (grabber && grabber != this && (grabber->keepMouseGrab() || grabber->keepTouchGrab()))) {
        m_touches.remove(point.id());
        m_ignored.insert(point.id());
        return false;
    }
    // The tile it came down on taps nothing: its tap handler only watches,
    // and a quick swipe can lift on that tile again, carried along under
    // the finger as the page follows it (Deck and Trackpad started so,
    // 2026-10-17).
    letGo(event, point);
    event->setExclusiveGrabber(point, this);
    m_pointId = point.id();
    m_strays.remove(m_pointId);
    m_swipe = touch;
    // Measure from here, so the page does not jump by the threshold.
    m_swipe.start = pos;
    m_swipe.startProgress = m_progress;
    // The other fingers down now (a resting thumb) tap nothing.
    for (auto it = m_touches.cbegin(); it != m_touches.cend(); ++it)
        if (it.key() != m_pointId)
            m_spent.insert(it.key());
    m_touches.clear();
    setDragging(true);
    emit dragStarted();
    return true;
}

bool PageSwipe::unseen(int id) const
{
    return !m_touches.contains(id) && !m_spent.contains(id) && !m_noTap.contains(id) && !m_ignored.contains(id);
}

bool PageSwipe::noTapNow(const QEventPoint &point) const
{
    return !m_strays.isEmpty() || m_clock.elapsed() < m_holdUntil || held(point.scenePressPosition());
}

void PageSwipe::letGo(QPointerEvent *event, const QEventPoint &point)
{
    // Its tap handlers tap nothing: one rule with Barry's scroll and pan
    // surfaces (panzoomarea.h).
    barry::takeFromTaps(event, point, this);
}

bool PageSwipe::atWork(QObject *grabber) const
{
    // A slider, a handler dragging, a list that can scroll: a finger at
    // work. A Flickable whose content fits (the apps, two rows) moves
    // nothing, though it takes the touch.
    if (!grabber || grabber == this)
        return false;
    // A list or view on Barry's touch rules (TouchScroll, PanZoomArea) holds
    // a touch from its press, but is at work only once it drags.
    if (auto *surface = qobject_cast<GestureSurface *>(grabber))
        return surface->dragging();
    if (grabber->inherits("QQuickFlickable")) {
        auto *item = static_cast<QQuickItem *>(grabber);
        return grabber->property("contentHeight").toReal() > item->height() + 0.5
               || grabber->property("contentWidth").toReal() > item->width() + 0.5;
    }
    return true;
}

void PageSwipe::stray(QPointerEvent *event, const QEventPoint &point)
{
    const int id = point.id();
    if (m_strays.contains(id))
        return;
    const QPointF d = point.scenePosition() - point.scenePressPosition();
    if (qMax(qAbs(d.x()), qAbs(d.y())) < m_threshold)
        return;
    if (atWork(event->exclusiveGrabber(point)))
        return;
    // It taps nothing, and this takes it (from a Flickable that cannot
    // scroll, say), so that the fingers coming down meanwhile pass here too.
    // Nor do the fingers down now (a resting thumb) tap anything.
    m_strays.insert(id);
    letGo(event, point);
    event->setExclusiveGrabber(point, this);
    for (auto it = m_touches.begin(); it != m_touches.end();) {
        if (it.key() == id) {
            ++it;
            continue;
        }
        m_spent.insert(it.key());
        it = m_touches.erase(it);
    }
}

void PageSwipe::move(const QEventPoint &point)
{
    const QPointF pos = point.scenePosition();
    track(m_swipe, pos);
    // One page either way from where the swipe began, within the pages.
    const int from = clampPage(qRound(m_swipe.startProgress));
    const qreal lo = qMax<qreal>(m_minPage, from - 1), hi = qMin<qreal>(m_maxPage, from + 1);
    setProgress(qBound<qreal>(lo, m_swipe.startProgress + (pos.x() - m_swipe.start.x()) / qMax<qreal>(1, width()), hi));
}

void PageSwipe::end()
{
    m_pointId = -1;
    if (!m_dragging)
        return;
    setDragging(false);
    m_quietUntil = m_clock.elapsed() + kSettleQuietMs;
    m_velocity = m_swipe.velocity;
    const int from = clampPage(qRound(m_swipe.startProgress));
    int target;
    if (m_velocity > kFling)
        target = from + 1;
    else if (m_velocity < -kFling)
        target = from - 1;
    else
        target = qRound(m_progress);
    settleTo(qBound(from - 1, target, from + 1));
}

void PageSwipe::cancel()
{
    m_pointId = -1;
    m_touches.clear();
    m_spent.clear();
    m_ignored.clear();
    m_strays.clear();
    m_noTap.clear();
    if (m_dragging) {
        setDragging(false);
        settleTo(qRound(m_progress));
    }
}

bool PageSwipe::handle(QPointerEvent *event)
{
    if (!m_swipeEnabled || !isVisible() || !isEnabled()) {
        cancel();
        return false;
    }
    // The first finger down: none left from before.
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::MouseButtonPress) {
        m_spent.clear();
        m_noTap.clear();
    }
    bool eat = false;
    for (const QEventPoint &point : event->points()) {
        const int id = point.id();
        switch (point.state()) {
        case QEventPoint::Pressed: {
            m_touches.remove(id);
            m_spent.remove(id);
            m_ignored.remove(id);
            m_strays.remove(id);
            m_noTap.remove(id);
            // During a swipe or as its page settles, a touch is no tap.
            if (m_dragging || m_clock.elapsed() < m_quietUntil) {
                m_spent.insert(id);
                eat = true;
                break;
            }
            const QPointF pos = point.scenePosition();
            m_touches.insert(id, Touch { pos, m_progress, pos, m_clock.elapsed(), 0 });
            // While another finger passes over, or held (Barry coming in), it
            // taps nothing: its press stops here, which makes this its grabber,
            // and it may still swipe.
            if (!m_strays.isEmpty() || m_clock.elapsed() < m_holdUntil || held(pos)) {
                m_noTap.insert(id);
                eat = true;
            }
            break;
        }
        case QEventPoint::Updated:
        case QEventPoint::Stationary:
            if (id == m_pointId) {
                move(point);
                eat = true;
                break;
            }
            if (unseen(id) && noTapNow(point))
                m_noTap.insert(id);
            if (m_spent.contains(id)) {
                letGo(event, point);
                eat = true;
                break;
            }
            if (m_noTap.contains(id))
                letGo(event, point);
            if (!m_dragging && point.state() == QEventPoint::Updated) {
                if (!m_ignored.contains(id)) {
                    auto it = m_touches.find(id);
                    // A finger whose press went past this (the second one down
                    // on a Flickable that holds the first): from where it came down.
                    if (it == m_touches.end())
                        it = m_touches.insert(id, Touch { point.scenePressPosition(), m_progress,
                                                          point.sceneLastPosition(), m_clock.elapsed(), 0 });
                    if (take(event, point, *it)) {
                        eat = true;
                        break;
                    }
                }
                // Not the swipe: maybe a finger passing over.
                stray(event, point);
            }
            break;
        case QEventPoint::Released: {
            if (id != m_pointId && unseen(id) && noTapNow(point))
                m_noTap.insert(id);
            m_touches.remove(id);
            m_ignored.remove(id);
            m_strays.remove(id);
            if (id == m_pointId) {
                end();
                eat = true;
            } else if (m_spent.contains(id) || m_noTap.contains(id)) {
                // Its tap handlers are cancelled. (Kept until it comes down
                // again: the release passes here more than once, to its
                // grabbers and then to what is under it.)
                letGo(event, point);
                eat = true;
            }
            break;
        }
        default:
            break;
        }
    }
    return eat || m_dragging;
}

bool PageSwipe::childMouseEventFilter(QQuickItem *, QEvent *event)
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
        if (pe->isSinglePointEvent() && static_cast<QMouseEvent *>(event)->source() != Qt::MouseEventNotSynthesized)
            return m_dragging;
        return handle(pe);
    }
    case QEvent::TouchCancel:
        cancel();
        return false;
    default:
        return false;
    }
}

void PageSwipe::touchEvent(QTouchEvent *event)
{
    if (event->type() == QEvent::TouchCancel) {
        cancel();
        return;
    }
    handle(event);
    event->setAccepted(true);
}

void PageSwipe::mousePressEvent(QMouseEvent *event)
{
    handle(event);
    event->setAccepted(true);
}

void PageSwipe::mouseMoveEvent(QMouseEvent *event)
{
    handle(event);
}

void PageSwipe::mouseReleaseEvent(QMouseEvent *event)
{
    handle(event);
}

void PageSwipe::touchUngrabEvent()
{
    cancel();
}

void PageSwipe::mouseUngrabEvent()
{
    cancel();
}
