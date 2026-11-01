// PanZoomGesture: Barry's pan, zoom and scroll, on frames of finger
// positions, as Android does them (ScaleGestureDetector with focal-point
// panning, as in a map; RecyclerView's drag and fling for a list). The QML
// items on it are PanZoomArea (a view any app binds to) and TouchScroll (the
// Launcher's scrolling lists), panzoomarea.h.
//
// The view: the content point at the area's center (x, y, content units)
// and the zoom (area pixels per content unit), so
//   area = (content - view) * zoom + size / 2.
// A list is a view at zoom 1 whose center moves along its scrolling axes.
//
// All fingers down drive one transform. Each frame, the content point that
// was under the fingers' midpoint moves to their new midpoint (a pan), and
// the zoom follows their spread (around that midpoint). A finger landing or
// lifting changes neither: the frame moves the view by the fingers that
// were down before and are still down, and the next frame measures from
// the fingers down now. So a pinch flows into a pan, and back, without a
// lift and without a jump.
//
// A touch is no drag until its midpoint has moved the touch slop (or, with
// two fingers, their spread has changed by it); then it moves from there,
// no jump by the slop. Up before that: one finger alone is a tap, held
// still for the long-press time a long press (and then nothing more). A
// tap may wait for a double tap (when double taps are on), which zooms in a
// step around it.
//
// Bounds, by mode. Center: the view's center stays in the bounds rect.
// Fill: the bounds rect is the content (a map, a picture), and the view
// stays inside it as a gallery's does: where the content is larger than the
// view (at the zoom of the moment) its edges never come inside the view's;
// where it is smaller, it is centered on that axis. Fill depends on the
// zoom and the size, so every pan, pinch, fling, eased zoom and wheel step
// clamps anew. Either way the edges stop the view hard, as a list's ends do
// (no rubber band, no overscroll).
//
// A wheel notch is a short eased move (or zoom step), so animating() turns
// on and off for it as for a fling; notches while it eases add up.
//
// Lifted while moving: a fling, the view gliding on as Android's scroller
// does (a spline that slows to a stop), from the finger's speed over its
// last moments. A finger that rested before lifting throws nothing; nor
// does a pinch that was still zooming, nor the finger left after a pinch
// unless it moved the slop by itself since. A touch stops a fling at once,
// and is a drag from there: no tap.
//
// Distances in area pixels; the touch rules (touchrules.h) are in design
// pixels, scaled by scale (area pixels per design pixel). Times in seconds:
// now on a monotonic clock, stamp the hardware's time of the frame (the
// speed of a throw is measured on it). Not thread-safe.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace barry {

class PanZoomGesture {
public:
    struct Point {
        double x, y;
    };
    struct View {
        double x, y, zoom;
    };

    enum class BoundsMode { Center, Fill };

    struct Callbacks {
        std::function<void(double, double)> tapped;
        std::function<void(double, double)> doubleTapped;
        std::function<void(double, double)> longPressed;
        std::function<void()> changed;  // the view moved
        // The touch became a drag (take it from what is under it).
        std::function<void()> dragStarted;
    };

    explicit PanZoomGesture(Callbacks callbacks);

    // The area's size and scale (area pixels per design pixel).
    void setSize(double width, double height);
    void setScale(double scale);
    // Zoom from min to max; a list: zoom off (its zoom stays 1).
    void setZoomRange(double min, double max);
    void setZoomEnabled(bool enabled);
    // The axes a drag moves (a list: its scrolling ones). None: never a drag.
    void setAxes(bool x, bool y);
    // Center: where the view's center may go, content units (max < min:
    // the middle). Fill: the content's rect, which the view stays inside
    // (see the header).
    void setBounds(double minX, double minY, double maxX, double maxY);
    void clearBounds();
    void setBoundsMode(BoundsMode mode);
    void setDoubleTapZoom(bool on);

    View view() const { return m_view; }
    // A new view from outside (an app's binding): kept within the zoom
    // range and bounds; a fling or zoom running stops.
    void setView(const View &view);
    // As setView, keeping a fling running (the outside only reports what it
    // shows: a list's own content position).
    void syncView(const View &view);

    // The fingers down now, by id (area pixels).
    void frame(const std::map<int, Point> &live, double now, double stamp);
    // The touch was taken away: no tap, no fling.
    void cancel();
    // A wheel: notches (up / away: positive) zoom around (x, y), or scroll
    // the content (x, y notches) where zoom is off; eased, as a short glide.
    void wheel(double x, double y, double notchesX, double notchesY, double now);
    // Zoom by factor around (x, y), eased (a button's zoom step).
    void zoomAround(double x, double y, double factor, double now);

    // When a long press or a waiting tap is due.
    std::optional<double> nextDue() const;
    void due(double now);
    // The glide (a fling, an eased zoom, a wheel's step) at now; false once
    // it is over.
    bool step(double now);
    void stop();

    bool active() const { return !m_live.empty(); }
    bool dragging() const { return m_state == Dragging; }
    bool animating() const { return m_anim != None; }

private:
    // Spent: no tap, no drag (it long-pressed, or moved where nothing moves).
    enum State { Idle, Pending, Dragging, Spent };
    enum Anim { None, Fling, Zoom, Scroll };
    struct Sample {
        double t, x, y, zoom;
    };

    double slop() const;
    void apply(double fx0, double fy0, double fx1, double fy1, double ratio);
    void clamp(View &v) const;
    void startZoom(double x, double y, double zoomTo, double dur, double now);
    void lift(double now, double stamp);
    void startFling(double vx, double vy, double now);
    void tap(double x, double y, double now);
    void flushTap();
    void startDrag();
    bool velocity(double stamp, double &vx, double &vy) const;
    void emitChanged();

    Callbacks m_cb;
    double m_w = 0, m_h = 0, m_scale = 1;
    double m_minZoom = 1, m_maxZoom = 1;
    bool m_zoomOn = true, m_axisX = true, m_axisY = true;
    bool m_bounded = false;
    BoundsMode m_boundsMode = BoundsMode::Center;
    double m_bx0 = 0, m_by0 = 0, m_bx1 = 0, m_by1 = 0;
    bool m_doubleTap = false;
    View m_view{0, 0, 1};

    std::map<int, Point> m_live;
    State m_state = Idle;
    int m_maxFingers = 0;
    double m_downAt = 0;
    double m_lastX = 0, m_lastY = 0;     // the fingers' midpoint last frame
    bool m_secondTap = false;            // came down while a tap waited
    double m_travelX = 0, m_travelY = 0, m_spread = 0;  // before the slop
    bool m_flingArmed = true;            // a lift throws (see the header)
    double m_soloX = 0, m_soloY = 0;     // the finger left after a pinch
    std::deque<Sample> m_samples;        // the view's pan (area px) and zoom
    double m_panX = 0, m_panY = 0;

    std::optional<double> m_longAt;
    std::optional<double> m_tapAt;      // a tap waiting for a double tap
    double m_tapX = 0, m_tapY = 0;

    Anim m_anim = None;
    double m_animStart = 0, m_animDur = 0;
    View m_from{0, 0, 1};
    double m_flingDist = 0, m_dirX = 0, m_dirY = 0;  // content units, unit vector
    double m_zoomTo = 1, m_zoomFx = 0, m_zoomFy = 0;
    bool m_wheelZoom = false;              // the Zoom glide is a wheel's
    double m_scrollToX = 0, m_scrollToY = 0;  // a wheel's Scroll glide
};

}  // namespace barry
