// PageSwipe: pages side by side, pulled in and pushed away sideways. In
// Barry Launcher they are the performance dashboard, the apps (home) and
// the quick settings, left to right: a left-to-right swipe from anywhere
// brings in the page to the left, right to left the one to the right.
//
// It watches its children's touches (and mouse, for a test without a
// touchscreen) without taking them: a tap on a tile is still a tap. A touch
// becomes a swipe only once it has moved `threshold` pixels (unset: Barry's
// touch slop, touchrules.h), mostly
// sideways, in a direction that moves the page; then the swipe takes the
// touch, and progress follows the finger. On release the page goes to the
// side it was thrown toward, or the nearer one, and settle() says where;
// the QML animates progress there (its look). A touch something keeps for
// itself (a slider with preventStealing) is never taken.
//
// Every finger is watched, and any of them can be the swipe: a thumb resting
// on the screen does not stop the other hand's swipe. Once one is, the
// others' touches are no taps (a thumb on a corner tile started its app as
// it lifted, 2026-10-17), nor are those that come down during the swipe or
// as the page settles. So it is with a finger moving that nothing uses (a
// swipe up over the tiles, a page swipe the wrong way): a hand passing over
// the screen, aiming at nothing, while a thumb holding the Thor rests on a
// tile (it started Deck, 2026-10-17).
//
// Barry coming in, touches are no taps for a moment (holdTaps: its tiles
// are still flying in); after a swipe up from the bottom edge opened it, a
// little longer where that thumb lifted (holdTapsBelow): on its way back to
// the edge it came down on Trackpad, Keyboard or Deck, the bottom row,
// a second after (2026-10-17).
//
// progress: which page shows, from minPage to maxPage. A left-to-right
// swipe raises it: the page at progress p is the one p places LEFT of page
// 0 (in Barry: 1 the dashboard, 0 home, -1 the quick settings). One swipe
// moves one page at most.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QPointF>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class QEventPoint;
class QPointerEvent;

class PageSwipe : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // minPage .. maxPage; the QML's settling animation writes it too, and
    // may pass a page a little (an overshoot).
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)
    // A finger is moving the pages.
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // The page going or gone to.
    Q_PROPERTY(int page READ page NOTIFY pageChanged)
    Q_PROPERTY(int minPage MEMBER m_minPage NOTIFY rangeChanged)
    Q_PROPERTY(int maxPage MEMBER m_maxPage NOTIFY rangeChanged)
    // Pixels a touch must travel before it is a swipe.
    Q_PROPERTY(qreal threshold MEMBER m_threshold NOTIFY thresholdChanged)
    // Off: touches pass untouched (an app on top, say).
    Q_PROPERTY(bool swipeEnabled MEMBER m_swipeEnabled NOTIFY swipeEnabledChanged)

public:
    explicit PageSwipe(QQuickItem *parent = nullptr);

    qreal progress() const { return m_progress; }
    void setProgress(qreal progress);
    bool dragging() const { return m_dragging; }
    int page() const { return m_page; }

    // Go to a page (settle() follows).
    Q_INVOKABLE void goTo(int page);
    // Touches coming down in the next ms are no taps.
    Q_INVOKABLE void holdTaps(int ms);
    // Nor, for ms, those coming down within radius to either side of
    // (x, y) and below it (or up to radius / 2 above; scene pixels), on the
    // page showing now. Either may still be a swipe.
    Q_INVOKABLE void holdTapsBelow(qreal x, qreal y, qreal radius, int ms);

signals:
    void progressChanged();
    void draggingChanged();
    void pageChanged();
    void rangeChanged();
    void thresholdChanged();
    void swipeEnabledChanged();
    // A finger took the page (stop any settling animation).
    void dragStarted();
    // Animate progress to target (a page); velocity in progress per second.
    // duration: a thrown page's, in ms, at the finger's pace (0: not thrown,
    // the page's usual pace); at most the usual one.
    void settle(qreal target, qreal velocity, qreal duration);

protected:
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void touchUngrabEvent() override;
    void mouseUngrabEvent() override;

private:
    // Returns true when the event is the swipe's (the filter then eats it).
    bool handle(QPointerEvent *event);
    struct Touch {
        QPointF start;        // scene position where it came down
        qreal startProgress;  // progress then
        QPointF last;
        qint64 lastMs;
        qreal velocity;       // progress per second, smoothed
    };
    void track(Touch &touch, const QPointF &pos);
    bool take(QPointerEvent *event, const QEventPoint &point, Touch &touch);
    void stray(QPointerEvent *event, const QEventPoint &point);
    bool atWork(QObject *grabber) const;
    bool unseen(int id) const;
    bool noTapNow(const QEventPoint &point) const;
    void letGo(QPointerEvent *event, const QEventPoint &point);
    bool held(const QPointF &pos) const;
    void move(const QEventPoint &point);
    void end();
    void cancel();
    void setDragging(bool dragging);
    void setPage(int page);
    void settleTo(int target);
    int clampPage(int page) const;
    qreal threshold() const;

    qreal m_progress = 0;
    bool m_dragging = false;
    int m_page = 0;
    int m_minPage = 0;
    int m_maxPage = 1;
    qreal m_threshold = -1;  // unset: Barry's touch slop
    bool m_swipeEnabled = true;

    // Touches down that may still become the swipe.
    QHash<int, Touch> m_touches;
    // Touches that are no taps: whatever they do, their events stop here
    // until they lift (and are kept until they come down again).
    QSet<int> m_spent;
    // Touches that are no swipe (the wrong way, or kept by a slider).
    QSet<int> m_ignored;
    // Touches moving that nothing uses: while one is down, no touch is a tap.
    QSet<int> m_strays;
    // Touches that may still swipe but tap nothing: their press and release
    // stop here.
    QSet<int> m_noTap;
    // The swipe's touch (-1: none), and how it moves.
    int m_pointId = -1;
    Touch m_swipe {};
    QElapsedTimer m_clock;
    // A touch coming down before this (ms on m_clock) is no tap: the page
    // is still settling.
    qint64 m_quietUntil = 0;
    // holdTaps(): a touch coming down before this (ms on m_clock) is no tap.
    qint64 m_holdUntil = 0;
    // holdTapsBelow(): until when (ms on m_clock), where and how wide.
    qint64 m_heldUntil = 0;
    QPointF m_heldAt;
    qreal m_heldRadius = 0;
    int m_heldPage = 0;  // the page it holds on (only there)
    qreal m_velocity = 0;      // the swipe's, as it was released
};
