// Barry's pan, zoom and scroll surfaces: PanZoomGesture (panzoom.h, the
// Launcher's touch rules) under QML. The QML binds to what they show and
// draws it; no touch math or tuning lives in it (GESTURES.md).
//
// PanZoomArea: a view any app binds to (a map, a picture): centerX, centerY
// (the content point at the area's center, content units) and zoom (area
// pixels per content unit), so an app draws content at
//   area = (content - center) * zoom + size / 2.
// One or more fingers pan and pinch it, a lift while moving flings it, taps,
// double taps and long presses are reported where they were (area pixels).
// A mouse drags it as a finger; a wheel zooms around the pointer. bounds
// keeps it in place, by boundsMode: Center keeps the center in the rect;
// Fill makes the rect the content (a map, a picture) and keeps the view
// inside it, as a gallery does (the content fills the view where it is
// larger, and is centered where it is smaller), at every zoom.
//
// TouchScroll: the Launcher's scrolling lists. It goes around a Flickable,
// which keeps its look (content, clip, size) and stops taking input itself;
// TouchScroll scrolls it (contentX / contentY) by the same rules: drag past
// the slop, fling, a touch stops a fling, the bounds stop it (no bounce).
//
//   TouchScroll {
//       anchors.fill: parent
//       Flickable { anchors.fill: parent; contentHeight: ...; clip: true; ... }
//   }
//
// Both watch their children's touches without taking them: a tap on a tile
// or a button inside is that child's, exactly as before. A touch becomes
// theirs once it is a drag (the core's slop, along an axis that moves);
// then it is taken from what is under it (its tap handlers tap nothing) and
// kept (a page swipe around does not take it back). A touch that catches a
// fling is a drag from the start. A touch something else takes (a page
// swipe around, a slider inside) is let go.
#pragma once

#include "panzoom.h"

#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <map>

class QEventPoint;
class QPointerEvent;
class QQuickWindow;

namespace barry {
// Takes point from the tap handlers watching it (they tap nothing as it
// lifts) for taker, as PageSwipe and these surfaces do.
void takeFromTaps(QPointerEvent *event, const QEventPoint &point, QObject *taker);
}  // namespace barry

// What PanZoomArea and TouchScroll share: the touches, to and from the core.
class GestureSurface : public QQuickItem {
    Q_OBJECT
    QML_ANONYMOUS
    // Off: touches pass untouched.
    Q_PROPERTY(bool interactive READ interactive WRITE setInteractive NOTIFY interactiveChanged)
    // Fingers (or the mouse) down on it.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // A finger is moving the view.
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // Dragging, or gliding on (a fling, an eased zoom).
    Q_PROPERTY(bool moving READ moving NOTIFY movingChanged)

public:
    explicit GestureSurface(QQuickItem *parent = nullptr);
    ~GestureSurface() override;

    bool interactive() const { return m_interactive; }
    void setInteractive(bool interactive);
    bool active() const { return m_active; }
    bool dragging() const { return m_dragging; }
    bool moving() const { return m_moving; }

signals:
    void interactiveChanged();
    void activeChanged();
    void draggingChanged();
    void movingChanged();

protected:
    // The view and its limits, from outside (before each frame and step).
    virtual void syncIn() = 0;
    // The core's view moved: show it.
    virtual void showView() = 0;
    // A wheel the core uses (false: it passes on).
    virtual bool wheels() const { return true; }
    // A tap, a double tap or a long press, where (area pixels).
    enum Tap { Single, Double, Long };
    virtual void tap(Tap kind, double x, double y)
    {
        (void)kind;
        (void)x;
        (void)y;
    }

    // Area pixels per design pixel: the window as the Launcher's layout
    // (1240 x 1080 at 1).
    double designScale() const;
    void stopGlide();
    // The view glides (a fling, an eased zoom): frames until it stops.
    void kick();
    // active, dragging, moving and the timer, after the core changed.
    void refresh();

    bool eventFilter(QObject *watched, QEvent *event) override;
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void touchUngrabEvent() override;
    void mouseUngrabEvent() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

    barry::PanZoomGesture m_gesture;

private:
    bool press(QPointerEvent *event);
    void track(QPointerEvent *event);
    void run(QPointerEvent *event);
    bool mine(QObject *grabber) const;
    void cancel();
    void arm();
    void tick();
    void setWindow(QQuickWindow *window);

    std::map<int, barry::PanZoomGesture::Point> m_live;
    QPointer<QQuickWindow> m_window;
    QTimer m_due;
    bool m_interactive = true;
    bool m_active = false;
    bool m_dragging = false;
    bool m_moving = false;
};

class PanZoomArea : public GestureSurface {
    Q_OBJECT
    QML_ELEMENT
    // The content point at the area's center, content units.
    Q_PROPERTY(qreal centerX READ centerX WRITE setCenterX NOTIFY viewChanged)
    Q_PROPERTY(qreal centerY READ centerY WRITE setCenterY NOTIFY viewChanged)
    // Area pixels per content unit, within minZoom .. maxZoom.
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged)
    Q_PROPERTY(qreal minZoom READ minZoom WRITE setMinZoom NOTIFY zoomRangeChanged)
    Q_PROPERTY(qreal maxZoom READ maxZoom WRITE setMaxZoom NOTIFY zoomRangeChanged)
    // Content units; an empty rect: anywhere. By boundsMode: where the
    // center may go (Center), or the content's rect, which the view stays
    // inside (Fill).
    Q_PROPERTY(QRectF bounds READ bounds WRITE setBounds NOTIFY boundsChanged)
    Q_PROPERTY(BoundsMode boundsMode READ boundsMode WRITE setBoundsMode NOTIFY boundsModeChanged)
    // A double tap zooms in a step around it (and taps wait a moment for a
    // second one); off: taps are at once, no double taps.
    Q_PROPERTY(bool doubleTapZoom READ doubleTapZoom WRITE setDoubleTapZoom NOTIFY doubleTapZoomChanged)

public:
    // Center (the default): bounds holds the center. Fill: bounds is the
    // content; the view shows no more than it, and centers it where it is
    // smaller than the view.
    enum BoundsMode { Center, Fill };
    Q_ENUM(BoundsMode)

    explicit PanZoomArea(QQuickItem *parent = nullptr);

    qreal centerX() const { return m_gesture.view().x; }
    qreal centerY() const { return m_gesture.view().y; }
    qreal zoom() const { return m_gesture.view().zoom; }
    void setCenterX(qreal x);
    void setCenterY(qreal y);
    void setZoom(qreal zoom);
    qreal minZoom() const { return m_minZoom; }
    qreal maxZoom() const { return m_maxZoom; }
    void setMinZoom(qreal zoom);
    void setMaxZoom(qreal zoom);
    QRectF bounds() const { return m_bounds; }
    void setBounds(const QRectF &bounds);
    BoundsMode boundsMode() const { return m_boundsMode; }
    void setBoundsMode(BoundsMode mode);
    bool doubleTapZoom() const { return m_doubleTapZoom; }
    void setDoubleTapZoom(bool on);

    // Area pixels to content units and back.
    Q_INVOKABLE QPointF mapToContent(qreal x, qreal y) const;
    Q_INVOKABLE QPointF mapFromContent(qreal x, qreal y) const;
    // Zoom by factor around (x, y) (area pixels; the center when left
    // out), eased: a zoom button's step.
    Q_INVOKABLE void zoomBy(qreal factor);
    Q_INVOKABLE void zoomBy(qreal factor, qreal x, qreal y);

signals:
    void viewChanged();
    void zoomRangeChanged();
    void boundsChanged();
    void boundsModeChanged();
    void doubleTapZoomChanged();
    // Where, area pixels.
    void tapped(qreal x, qreal y);
    void doubleTapped(qreal x, qreal y);
    void longPressed(qreal x, qreal y);

protected:
    void syncIn() override;
    void showView() override;
    void tap(Tap kind, double x, double y) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void limits();
    void set(barry::PanZoomGesture::View view);

    qreal m_minZoom = 0.1;
    qreal m_maxZoom = 10;
    QRectF m_bounds;
    BoundsMode m_boundsMode = Center;
    bool m_doubleTapZoom = false;
};

class TouchScroll : public GestureSurface {
    Q_OBJECT
    QML_ELEMENT
    // The Flickable it scrolls; left out: its first child that is one.
    Q_PROPERTY(QQuickItem *flickable READ flickable WRITE setFlickable NOTIFY flickableChanged)

public:
    explicit TouchScroll(QQuickItem *parent = nullptr);

    QQuickItem *flickable() const { return m_flickable; }
    void setFlickable(QQuickItem *flickable);

signals:
    void flickableChanged();

protected:
    void componentComplete() override;
    void syncIn() override;
    void showView() override;
    bool wheels() const override;

private:
    QPointer<QQuickItem> m_flickable;
    bool m_axisX = false, m_axisY = false;
    double m_shownX = 0, m_shownY = 0;  // the content position last written
    bool m_shown = false;
};
