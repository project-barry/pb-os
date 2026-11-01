// touch-surfaces-test: TouchScroll and PanZoomArea (panzoomarea.h) in a
// window, offscreen, under Qt's own touch events: a list as the Launcher's
// Home has it (a Flickable inside, a tap handler on its content, PageSwipe
// around) and a pan-zoom view beside it.
//
//   touch-surfaces-test   (exit 0: all passed)
//
// Covers: a tap on the list's content still taps; a drag scrolls it (and
// taps nothing); a throw glides on; a touch stops the glide and taps
// nothing; a sideways swipe over the list is still the page swipe's; the
// view's tap, long press, pinch and a mouse drag.
#include "../../pageswipe.h"
#include "../../panzoomarea.h"

#include <QGuiApplication>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

const char QML[] = R"(
import QtQuick
import Barry.Input
Window {
    id: root
    width: 1240; height: 1080
    visible: true
    property int taps: 0
    property var events: []
    PageSwipe {
        objectName: "swipe"
        anchors.fill: parent
        minPage: -1; maxPage: 1; threshold: 24
        TouchScroll {
            objectName: "scroll"
            x: 0; y: 0; width: 600; height: 500
            Flickable {
                objectName: "flick"
                anchors.fill: parent
                contentWidth: width; contentHeight: 3000
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                Rectangle {
                    width: 600; height: 3000; color: "gray"
                    TapHandler { onTapped: root.taps++ }
                }
            }
        }
    }
    PanZoomArea {
        objectName: "map"
        x: 640; y: 0; width: 600; height: 600
        minZoom: 0.5; maxZoom: 4; zoom: 1
        onTapped: (x, y) => root.events.push("tap")
        onLongPressed: (x, y) => root.events.push("long")
    }
}
)";

QPointingDevice *touchscreen;
QQuickWindow *window;

void press(int id, QPoint at) { QTest::touchEvent(window, touchscreen).press(id, at, window); }
void move(int id, QPoint at) { QTest::touchEvent(window, touchscreen).move(id, at, window); }
void release(int id, QPoint at) { QTest::touchEvent(window, touchscreen).release(id, at, window); }

// A finger from a to b in steps, ms apart.
void stroke(int id, QPoint a, QPoint b, int steps, int ms, bool lift = true)
{
    press(id, a);
    for (int i = 1; i <= steps; i++) {
        QTest::qWait(ms);
        move(id, a + (b - a) * i / steps);
    }
    if (lift)
        release(id, b);
}

}  // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    qmlRegisterAnonymousType<GestureSurface>("Barry.Input", 1);
    qmlRegisterType<TouchScroll>("Barry.Input", 1, 0, "TouchScroll");
    qmlRegisterType<PanZoomArea>("Barry.Input", 1, 0, "PanZoomArea");
    qmlRegisterType<PageSwipe>("Barry.Input", 1, 0, "PageSwipe");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QML, QUrl(QStringLiteral("file:///touch_surfaces_test.qml")));
    QObject *root = component.create();
    if (!root) {
        std::printf("FAIL the test window: %s\n", qPrintable(component.errorString()));
        return 1;
    }
    window = qobject_cast<QQuickWindow *>(root);
    touchscreen = QTest::createTouchDevice();
    if (!QTest::qWaitForWindowExposed(window)) {
        std::printf("FAIL the test window is not shown\n");
        return 1;
    }
    auto *scroll = root->findChild<QQuickItem *>(QStringLiteral("scroll"));
    auto *flick = root->findChild<QQuickItem *>(QStringLiteral("flick"));
    auto *swipe = root->findChild<QQuickItem *>(QStringLiteral("swipe"));
    auto *map = root->findChild<QQuickItem *>(QStringLiteral("map"));
    const auto taps = [root] { return root->property("taps").toInt(); };
    const auto contentY = [flick] { return flick->property("contentY").toReal(); };
    const auto events = [root] { return root->property("events").toStringList(); };

    check(!flick->property("interactive").toBool(), "list: its Flickable takes no input itself");

    // A tap on the content.
    press(1, {300, 250});
    QTest::qWait(60);
    release(1, {300, 250});
    QTest::qWait(30);
    check(taps() == 1, "list: a tap on its content taps");
    check(contentY() == 0, "list: and scrolls nothing");

    // A slow drag up.
    stroke(1, {300, 450}, {300, 250}, 20, 16, false);
    const bool swiped = swipe->property("dragging").toBool();
    check(scroll->property("dragging").toBool(), "list: a drag up drags it");
    QTest::qWait(150);
    release(1, {300, 250});
    QTest::qWait(50);
    std::printf("     dragged 200 px: contentY %.1f\n", contentY());
    check(contentY() > 170 && contentY() <= 200, "list: the content followed the finger (less the slop)");
    check(taps() == 1, "list: a drag taps nothing");
    check(!swiped, "list: the page swipe left it alone");
    check(!scroll->property("moving").toBool(), "list: rested before lifting: no glide");

    // A throw.
    stroke(1, {300, 450}, {300, 210}, 8, 8);
    const qreal thrown = contentY();
    check(scroll->property("moving").toBool(), "list: a throw glides on");
    QTest::qWait(250);
    std::printf("     thrown: contentY %.1f at the lift, %.1f 250 ms later\n", thrown, contentY());
    check(contentY() > thrown + 20, "list: the glide moved the content on");

    // A touch on the glide.
    QTest::qWait(1000);
    stroke(1, {300, 450}, {300, 210}, 8, 8);
    QTest::qWait(40);
    press(1, {300, 250});
    QTest::qWait(16);
    const qreal caught = contentY();
    QTest::qWait(150);
    check(qAbs(contentY() - caught) < 0.5, "list: a touch stops the glide at once");
    release(1, {300, 250});
    QTest::qWait(30);
    check(taps() == 1, "list: the touch that stopped it taps nothing");
    QTest::qWait(300);
    press(1, {300, 250});
    QTest::qWait(60);
    release(1, {300, 250});
    QTest::qWait(30);
    check(taps() == 2, "list: once still, a tap taps again");

    // Sideways over the list: the page swipe's.
    stroke(1, {300, 250}, {460, 250}, 10, 16, false);
    check(swipe->property("dragging").toBool(), "list: a sideways swipe over it is the page swipe's");
    check(!scroll->property("dragging").toBool(), "list: and not the list's");
    release(1, {460, 250});
    QTest::qWait(500);

    // The view: a tap, a long press.
    press(2, {940, 300});
    QTest::qWait(60);
    release(2, {940, 300});
    QTest::qWait(30);
    check(events() == QStringList{"tap"}, "view: a tap");
    press(2, {940, 300});
    QTest::qWait(700);
    release(2, {940, 300});
    QTest::qWait(30);
    check(events() == (QStringList{"tap", "long"}), "view: a long press, and no tap after it");

    // A pinch, then a pan without lifting.
    QTest::touchEvent(window, touchscreen).press(3, {840, 300}, window).press(4, {1040, 300}, window);
    for (int i = 1; i <= 10; i++) {
        QTest::qWait(16);
        QTest::touchEvent(window, touchscreen).move(3, {840 - 10 * i, 300}, window).move(4, {1040 + 10 * i, 300}, window);
    }
    const qreal zoomed = map->property("zoom").toReal();
    std::printf("     pinched 200 -> 400 px: zoom %.3f\n", zoomed);
    check(zoomed > 1.5 && zoomed <= 2.0, "view: a pinch zooms");
    const qreal cx = map->property("centerX").toReal();
    for (int i = 1; i <= 5; i++) {
        QTest::qWait(16);
        QTest::touchEvent(window, touchscreen).move(3, {740 + 10 * i, 300}, window).move(4, {1140 + 10 * i, 300}, window);
    }
    check(qAbs(map->property("zoom").toReal() - zoomed) < 1e-6, "view: on into a pan, the zoom holds");
    check(qAbs(map->property("centerX").toReal() - (cx - 50 / zoomed)) < 0.01, "view: and the content follows");
    QTest::qWait(150);
    QTest::touchEvent(window, touchscreen).release(3, {790, 300}, window).release(4, {1190, 300}, window);
    QTest::qWait(30);
    check(events().size() == 2, "view: a pinch taps nothing");

    // A mouse drags it as a finger.
    const qreal mx = map->property("centerX").toReal();
    QTest::mousePress(window, Qt::LeftButton, {}, {900, 400});
    for (int i = 1; i <= 10; i++)
        QTest::mouseMove(window, {900 + 10 * i, 400}, 16);
    QTest::qWait(150);
    QTest::mouseRelease(window, Qt::LeftButton, {}, {1000, 400});
    const qreal dragged = (mx - map->property("centerX").toReal()) * map->property("zoom").toReal();
    std::printf("     mouse dragged 100 px: the view moved %.1f px\n", dragged);
    check(dragged > 70 && dragged <= 100, "view: a mouse drag pans it");

    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
    delete root;
    return failures ? 1 : 0;
}
