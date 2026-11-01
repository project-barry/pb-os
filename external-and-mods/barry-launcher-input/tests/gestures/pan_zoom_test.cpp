// pan-zoom-test: PanZoomGesture (panzoom.h), the Launcher's pan, zoom and
// scroll rules, on frames of finger positions: no Qt, no screen.
//
//   pan-zoom-test   (exit 0: all passed)
//
// Covers: the slop (a tap stays a tap), a pan, a pinch (zoom around the
// midpoint), a pinch flowing into a pan without a lift, a finger landing or
// lifting without a jump, a quick pinch's release (no fling from the
// finger left, unless it moves), tap against long press, a fling's glide and
// a touch stopping it, the zoom's clamps, a list's bounds and axes, the
// wheel (eased, notches adding up), double taps, Fill bounds (the content
// fills the view at every zoom, through pan, pinch, fling, eased zoom and
// wheel).
#include "../../panzoom.h"
#include "../../touchrules.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using barry::PanZoomGesture;
namespace touch = barry::touch;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        failures++;
}

bool near(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

// A gesture on a 1000 x 800 area at scale 1, zoom 0.25 .. 8, the clock its
// own (seconds).
struct Rig {
    std::vector<std::string> log;
    double t = 100;
    PanZoomGesture g{{
        [this](double x, double y) { log.push_back("tap " + std::to_string(int(x)) + "," + std::to_string(int(y))); },
        [this](double, double) { log.push_back("double"); },
        [this](double, double) { log.push_back("long"); },
        nullptr,
        [this] { log.push_back("drag"); },
    }};
    std::map<int, PanZoomGesture::Point> fingers;

    Rig(bool zoom = true)
    {
        g.setSize(1000, 800);
        g.setScale(1);
        g.setZoomEnabled(zoom);
        g.setZoomRange(zoom ? 0.25 : 1, zoom ? 8 : 1);
        g.setView({0, 0, 1});
    }
    // The fingers as they are now, dt seconds after the last frame.
    void frame(double dt = 0.008)
    {
        t += dt;
        g.due(t);
        g.frame(fingers, t, t);
    }
    void down(int id, double x, double y, double dt = 0.008)
    {
        fingers[id] = {x, y};
        frame(dt);
    }
    void move(int id, double x, double y, double dt = 0.008)
    {
        fingers[id] = {x, y};
        frame(dt);
    }
    void up(int id, double dt = 0.008)
    {
        fingers.erase(id);
        frame(dt);
    }
    void wait(double s)
    {
        t += s;
        g.due(t);
    }
    bool has(const std::string &what) const
    {
        for (const auto &l : log)
            if (l.rfind(what, 0) == 0)
                return true;
        return false;
    }
    // The content point at area (x, y).
    double cx(double x) const { return g.view().x + (x - 500) / g.view().zoom; }
    double cy(double y) const { return g.view().y + (y - 400) / g.view().zoom; }
    // Run the glide to its end (or for at most s seconds).
    void glide(double s = 10)
    {
        for (double end = t + s; g.animating() && t < end;) {
            t += 1.0 / 120;
            g.step(t);
        }
    }
};

void slopAndTap()
{
    Rig r;
    r.down(1, 500, 400);
    r.move(1, 510, 405);  // inside the slop
    check(near(r.g.view().x, 0) && near(r.g.view().y, 0), "slop: a finger wandering 11 px moves nothing");
    check(!r.g.dragging(), "slop: not a drag yet");
    r.up(1);
    check(r.has("tap 510,405"), "slop: lifted, it is a tap where it lifted");
    check(!r.g.animating(), "slop: a tap throws nothing");
}

void pan()
{
    Rig r;
    r.down(1, 500, 400);
    r.move(1, 520, 400);  // past the slop: a drag from here
    check(r.has("drag"), "pan: past the slop it is a drag");
    check(near(r.g.view().x, 0), "pan: no jump by the slop");
    r.move(1, 600, 450);
    check(near(r.g.view().x, -80) && near(r.g.view().y, -50), "pan: the content follows the finger");
    r.wait(0.1);  // rests, then lifts
    r.up(1);
    check(!r.g.animating(), "pan: a finger that rested before lifting throws nothing");
    check(!r.has("tap"), "pan: a drag is no tap");
}

void pinch()
{
    Rig r;
    r.down(1, 400, 400);
    r.down(2, 600, 400);
    r.move(1, 380, 400);
    r.move(2, 620, 400);  // spread +20: past the slop
    check(r.g.dragging(), "pinch: spreading past the slop is a drag");
    const double underX = r.cx(500), underY = r.cy(400);
    r.move(1, 260, 400);
    r.move(2, 740, 400);  // spread 120 -> 240 over two frames
    check(near(r.g.view().zoom, 2, 1e-9), "pinch: zoom follows the fingers' spread (x2)");
    check(near(r.cx(500), underX, 1e-9) && near(r.cy(400), underY, 1e-9),
          "pinch: the point under the midpoint stays under it");

    // On into a pan, the same fingers, no lift.
    const double before = r.g.view().x;
    r.move(1, 360, 400);
    r.move(2, 840, 400);  // both 100 to the right (midpoint +50 per frame)
    check(near(r.g.view().zoom, 2, 1e-9), "pinch -> pan: the zoom holds");
    check(near(r.g.view().x, before - 100 / 2.0, 1e-9), "pinch -> pan: the content follows the midpoint");

    // A finger lifts: nothing jumps; the one left pans on.
    const auto v = r.g.view();
    r.up(2);
    check(near(r.g.view().x, v.x) && near(r.g.view().y, v.y) && near(r.g.view().zoom, v.zoom),
          "lift: one finger lifting moves nothing");
    r.move(1, 400, 420);
    check(near(r.g.view().x, v.x - 40 / 2.0, 1e-9) && near(r.g.view().y, v.y - 20 / 2.0, 1e-9),
          "lift: the finger left pans from where it is");
    // A finger lands far away: nothing jumps either.
    const auto w = r.g.view();
    r.down(3, 900, 100);
    check(near(r.g.view().x, w.x) && near(r.g.view().y, w.y) && near(r.g.view().zoom, w.zoom),
          "land: a finger landing moves nothing");
    r.move(3, 900, 100);
    check(near(r.g.view().zoom, w.zoom), "land: still, it zooms nothing");
    r.up(1);
    r.up(3);
    check(!r.has("tap"), "pinch: no tap");
}

void quickPinchRelease()
{
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 6; i++) {
            r.move(1, 400 - 15 * i, 400 + 3 * i, 0.004);
            r.move(2, 600 + 15 * i, 400 + 3 * i, 0.004);
        }
        r.up(2, 0.004);
        r.move(1, 309, 419, 0.004);  // the finger left jiggles a little
        r.up(1, 0.004);
        check(!r.g.animating(), "quick pinch: the finger left after it throws nothing");
    }
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 6; i++) {
            r.move(1, 400 - 15 * i, 400, 0.004);  // the midpoint drifts too
            r.move(2, 600 + 25 * i, 400, 0.004);
        }
        r.fingers.clear();
        r.frame(0.004);  // both lift at once, still spreading
        check(!r.g.animating(), "quick pinch: both lifting while zooming throws nothing");
    }
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 4; i++) {
            r.move(1, 400 - 15 * i, 400, 0.008);
            r.move(2, 600 + 15 * i, 400, 0.008);
        }
        r.up(2);
        for (int i = 1; i <= 8; i++)
            r.move(1, 340, 400 + 12 * i, 0.008);  // then really moves, fast
        r.up(1, 0.008);
        check(r.g.animating(), "quick pinch: the finger left throws once it moved by itself");
    }
}

void tapAndLongPress()
{
    Rig r;
    r.down(1, 300, 300);
    r.wait(touch::LONG_PRESS_S - 0.05);
    check(!r.has("long"), "long press: not before its time");
    r.wait(0.06);
    check(r.has("long"), "long press: held still for the long-press time");
    r.move(1, 400, 300);
    check(!r.g.dragging() && near(r.g.view().x, 0), "long press: then nothing moves");
    r.up(1);
    check(!r.has("tap"), "long press: no tap after it");

    Rig q;
    q.down(1, 300, 300);
    q.down(2, 500, 300);
    q.wait(1);
    q.up(1);
    q.up(2);
    check(!q.has("long") && !q.has("tap"), "two fingers held: no long press, no tap");

    Rig n;
    n.down(1, 300, 300);
    n.up(1, 0.1);
    check(n.has("tap 300,300"), "tap: down and up");
    check(n.g.nextDue() == std::nullopt, "tap: nothing left due (double taps off)");
}

void flingAndCatch()
{
    Rig r;
    r.down(1, 500, 700);
    double y = 700;
    for (int i = 0; i < 12; i++) {
        y -= 12;  // 1500 px/s upward
        r.move(1, 500, y);
    }
    const double atRelease = r.g.view().y;
    r.up(1);
    check(r.g.animating(), "fling: a throw glides on");
    // Speeds along the glide: falling, never reversing.
    double last = r.g.view().y, lastStep = 1e9;
    bool slowing = true, onward = true;
    const double t0 = r.t;
    for (int i = 0; i < 240 && r.g.animating(); i++) {
        r.t += 1.0 / 120;
        r.g.step(r.t);
        const double step = r.g.view().y - last;
        if (step < -1e-9)
            onward = false;
        if (step > lastStep + 1e-6)
            slowing = false;
        lastStep = step;
        last = r.g.view().y;
    }
    check(onward, "fling: the content goes on the way it was thrown");
    check(slowing, "fling: and slows down");
    r.glide();
    const double dist = r.g.view().y - atRelease;
    // Android's scroller for 1500 px/s at 420 ppi: about 193 px over about
    // 0.37 s.
    std::printf("     fling 1500 px/s: %.0f px in %.2f s\n", dist, r.t - t0);
    check(dist > 160 && dist < 230, "fling: Android's distance for its speed");
    check(r.t - t0 > 0.3 && r.t - t0 < 0.45, "fling: Android's duration for its speed");
    check(!r.g.animating(), "fling: stops by itself");

    // A touch stops a glide at once, and is no tap.
    Rig c;
    c.down(1, 500, 700);
    for (int i = 0; i < 10; i++)
        c.move(1, 500, 700 - 20 * (i + 1));
    c.up(1);
    for (int i = 0; i < 6; i++) {
        c.t += 1.0 / 120;
        c.g.step(c.t);
    }
    check(c.g.animating(), "catch: gliding");
    c.log.clear();
    c.down(2, 300, 300);
    const double held = c.g.view().y;
    check(!c.g.animating(), "catch: a touch stops it at once");
    check(c.g.dragging(), "catch: and is a drag from there (it takes the touch)");
    c.t += 0.05;
    c.g.step(c.t);
    check(near(c.g.view().y, held), "catch: nothing glides on");
    c.up(2, 0.05);
    check(!c.has("tap"), "catch: the touch that stopped it taps nothing");
}

void clamps()
{
    Rig r;
    r.down(1, 490, 400);
    r.down(2, 510, 400);
    r.move(1, 0, 400);
    r.move(2, 1000, 400);
    r.move(1, -5000, 400);
    r.move(2, 6000, 400);
    check(near(r.g.view().zoom, 8), "clamp: zoom stops at the max");
    r.up(1);
    r.up(2);
    r.g.wheel(500, 400, 0, -40, r.t);
    r.glide();
    check(near(r.g.view().zoom, 0.25), "clamp: wheel out stops at the min");
    const double under = r.cx(200);
    r.g.wheel(200, 400, 0, 1, r.t);
    r.glide();
    check(near(r.g.view().zoom, 0.25 * touch::WHEEL_ZOOM, 1e-9), "wheel: a notch zooms a step");
    check(near(r.cx(200), under, 1e-9), "wheel: around the pointer");
}

void list()
{
    // A list: zoom off, down only, content 0 .. 2000 (the center 400 .. 1600).
    Rig r(false);
    r.g.setAxes(false, true);
    r.g.setBounds(500, 400, 500, 1600);
    r.g.setView({500, 400, 1});
    r.down(1, 500, 600);
    r.move(1, 540, 600);  // sideways: nothing to move that way
    r.move(1, 560, 600);
    check(!r.g.dragging(), "list: sideways is no scroll (a page swipe's)");
    r.up(1);
    check(!r.has("tap"), "list: nor a tap");

    r.down(1, 500, 600);
    r.move(1, 500, 580);
    r.move(1, 500, 500);
    check(near(r.g.view().y, 480) && near(r.g.view().x, 500), "list: up scrolls down the content, along y only");
    r.move(1, 500, 900);
    check(near(r.g.view().y, 400), "list: stops at the top (no overscroll)");
    r.up(1, 0.1);

    // Thrown at the end: stops there, early.
    r.g.setView({500, 1500, 1});
    r.down(1, 500, 700);
    for (int i = 0; i < 10; i++)
        r.move(1, 500, 700 - 30 * (i + 1));
    r.up(1);
    check(r.g.animating(), "list: thrown toward the end");
    const double t0 = r.t;
    r.glide();
    check(near(r.g.view().y, 1600), "list: the glide stops at the end");
    check(r.t - t0 < 0.5, "list: at once there, not running on");

    // A wheel notch: WHEEL_PX.
    r.g.setView({500, 1000, 1});
    r.g.wheel(500, 400, 0, -1, r.t);
    r.glide();
    check(near(r.g.view().y, 1000 + touch::WHEEL_PX), "list: a wheel notch down scrolls WHEEL_PX");
}

void wheelEases()
{
    // A list's notch: a short glide, so the view is moving, then not.
    Rig r(false);
    r.g.setAxes(false, true);
    r.g.setBounds(500, 400, 500, 1600);
    r.g.setView({500, 1000, 1});
    r.g.wheel(500, 400, 0, -1, r.t);
    check(r.g.animating(), "wheel: a notch on a list is a glide (moving)");
    check(near(r.g.view().y, 1000), "wheel: no jump at the notch");
    r.t += touch::WHEEL_ANIM_S / 2;
    r.g.step(r.t);
    const double mid = r.g.view().y;
    check(mid > 1000 + touch::WHEEL_PX / 2 && mid < 1000 + touch::WHEEL_PX, "wheel: eased out, most of the way early");
    r.g.wheel(500, 400, 0, -1, r.t);  // a second notch while it eases
    r.glide();
    check(near(r.g.view().y, 1000 + 2 * touch::WHEEL_PX), "wheel: notches while it eases add up");
    check(!r.g.animating(), "wheel: and it stops (moving off)");
    r.g.setView({500, 1600, 1});
    r.g.wheel(500, 400, 0, -1, r.t);
    check(!r.g.animating() && near(r.g.view().y, 1600), "wheel: at the end, nothing moves");
    // Toward the end by more than is left: stops there.
    r.g.setView({500, 1550, 1});
    r.g.wheel(500, 400, 0, -3, r.t);
    r.glide();
    check(near(r.g.view().y, 1600), "wheel: notches past the end stop at it");

    // A view's notch: an eased zoom step around the pointer.
    Rig z;
    const double under = z.cx(200);
    z.g.wheel(200, 400, 0, 1, z.t);
    check(z.g.animating() && near(z.g.view().zoom, 1), "wheel: a zoom notch is a glide too");
    z.t += 0.01;
    z.g.wheel(200, 400, 0, 1, z.t);
    z.glide();
    check(near(z.g.view().zoom, touch::WHEEL_ZOOM * touch::WHEEL_ZOOM, 1e-9), "wheel: zoom notches add up");
    check(near(z.cx(200), under, 1e-9), "wheel: still around the pointer");
    check(!z.g.animating(), "wheel: the zoom step stops");
    z.g.setView({0, 0, 8});
    z.g.wheel(200, 400, 0, 1, z.t);
    check(!z.g.animating(), "wheel: at the max zoom, nothing moves");

    // A touch stops a wheel's glide, as a fling's.
    Rig c(false);
    c.g.setAxes(false, true);
    c.g.setBounds(500, 400, 500, 1600);
    c.g.setView({500, 1000, 1});
    c.g.wheel(500, 400, 0, -2, c.t);
    c.t += 0.02;
    c.g.step(c.t);
    c.down(1, 500, 500);
    const double held = c.g.view().y;
    check(!c.g.animating(), "wheel: a touch stops its glide");
    c.t += 0.2;
    c.g.step(c.t);
    check(near(c.g.view().y, held), "wheel: nothing glides on after the touch");
    c.up(1, 0.05);
}

// The content (Fill bounds 0 .. 2000 x 0 .. 1600) fills the 1000 x 800
// view: its edges outside the view's, or centered where it is the smaller.
bool fills(const Rig &r)
{
    const auto v = r.g.view();
    const auto axis = [](double c, double lo, double hi, double size, double zoom) {
        const double a = (lo - c) * zoom + size / 2, b = (hi - c) * zoom + size / 2;
        if (b - a <= size + 1e-9)
            return near(a + b, size, 1e-6);
        return a <= 1e-6 && b >= size - 1e-6;
    };
    return axis(v.x, 0, 2000, 1000, v.zoom) && axis(v.y, 0, 1600, 800, v.zoom);
}

void fillBounds()
{
    const auto rig = [](Rig &r) {
        r.g.setBoundsMode(PanZoomGesture::BoundsMode::Fill);
        r.g.setBounds(0, 0, 2000, 1600);
        r.g.setView({1000, 800, 1});
    };
    Rig r;
    rig(r);
    r.g.setView({0, 0, 1});
    check(near(r.g.view().x, 500) && near(r.g.view().y, 400), "fill: the view stops at the content's corner");
    r.g.setView({5000, 5000, 1});
    check(near(r.g.view().x, 1500) && near(r.g.view().y, 1200), "fill: and at the far corner");
    r.g.setView({0, 0, 0.25});
    check(near(r.g.view().x, 1000) && near(r.g.view().y, 800), "fill: content smaller than the view: centered");
    r.g.setView({0, 0, 0.625});  // 1250 x 1000 on 1000 x 800: larger both ways
    check(near(r.g.view().x, 800) && near(r.g.view().y, 640), "fill: the clamp follows the zoom");
    r.g.setView({0, 0, 0.55});  // 1100 x 880: larger across, narrowly
    check(fills(r) && near(r.g.view().x, 1000 - 50 / 0.55), "fill: just larger: its edge at the view's");

    // A pan past the corner stops at it, and pans back at once.
    r.g.setView({600, 500, 1});
    r.down(1, 500, 400);
    r.move(1, 520, 420);
    r.move(1, 900, 800);
    check(near(r.g.view().x, 500) && near(r.g.view().y, 400), "fill: a pan stops at the content's edges");
    r.move(1, 850, 750);
    check(near(r.g.view().x, 550) && near(r.g.view().y, 450), "fill: and moves back at once (no slack)");
    r.wait(0.1);
    r.up(1);

    // A pinch out at the corner: the content fills the view all the way, and
    // centers once it is smaller.
    Rig p;
    rig(p);
    p.g.setView({500, 400, 1});
    p.down(1, 100, 100);
    p.down(2, 300, 300);
    bool always = true;
    for (int i = 1; i <= 12; i++) {
        p.move(1, 100 + 7 * i, 100 + 7 * i);
        p.move(2, 300 - 7 * i, 300 - 7 * i);
        always = always && fills(p);
    }
    check(always, "fill: pinched out at the corner, the content fills the view every frame");
    check(p.g.view().zoom < 0.5 && near(p.g.view().x, 1000) && near(p.g.view().y, 800),
          "fill: pinched smaller than the view: centered");
    p.up(1);
    p.up(2);
    // And in again: off-center, kept filling.
    p.down(1, 400, 300);
    p.down(2, 450, 350);
    always = true;
    for (int i = 1; i <= 20; i++) {
        p.move(1, 400 - 15 * i, 300 - 15 * i);
        p.move(2, 450 + 15 * i, 350 + 15 * i);
        always = always && fills(p);
    }
    check(always && p.g.view().zoom > 1, "fill: pinched in, it fills the view every frame");
    p.up(1);
    p.up(2);

    // A fling toward an edge stops at it, early.
    Rig f;
    rig(f);
    f.g.setView({900, 800, 1});
    f.down(1, 300, 400);
    for (int i = 0; i < 10; i++)
        f.move(1, 300 + 30 * (i + 1), 400);
    f.up(1);
    check(f.g.animating(), "fill: thrown toward the left edge");
    const double t0 = f.t;
    f.glide();
    check(near(f.g.view().x, 500), "fill: the glide stops at the edge");
    check(f.t - t0 < 0.5, "fill: at once there, not running on");

    // An eased zoom out (a button) and the wheel: every step fills.
    Rig e;
    rig(e);
    e.g.setView({500, 400, 2});
    e.g.zoomAround(0, 0, 0.25, e.t);
    always = true;
    while (e.g.animating()) {
        e.t += 1.0 / 120;
        e.g.step(e.t);
        always = always && fills(e);
    }
    check(always && near(e.g.view().zoom, 0.5), "fill: an eased zoom out fills the view every step");
    e.g.setView({1500, 1200, 1});
    e.g.wheel(1000, 800, 0, -3, e.t);
    always = true;
    while (e.g.animating()) {
        e.t += 1.0 / 120;
        e.g.step(e.t);
        always = always && fills(e);
    }
    check(always && near(e.g.view().zoom, std::pow(touch::WHEEL_ZOOM, -3), 1e-9),
          "fill: wheel zoom steps fill the view every step");

    // Center (the default) is as before: the center goes to the rect's edge.
    Rig c;
    c.g.setBounds(0, 0, 2000, 1600);
    c.g.setView({-100, -100, 1});
    check(near(c.g.view().x, 0) && near(c.g.view().y, 0), "center: the default keeps only the center in bounds");
}

void doubleTap()
{
    Rig r;
    r.g.setDoubleTapZoom(true);
    r.down(1, 300, 300);
    r.up(1, 0.05);
    check(!r.has("tap"), "double tap on: a tap waits for a second one");
    r.down(1, 310, 305, 0.1);
    r.up(1, 0.05);
    check(r.has("double") && !r.has("tap"), "double tap: two taps are one double tap");
    const double under = r.cx(310);
    r.glide();
    check(near(r.g.view().zoom, touch::DOUBLE_TAP_ZOOM), "double tap: zooms in a step");
    check(near(r.cx(310), under, 1e-9), "double tap: around it");

    Rig s;
    s.g.setDoubleTapZoom(true);
    s.down(1, 300, 300);
    s.up(1, 0.05);
    s.wait(touch::TAP_S + 0.01);
    check(s.has("tap 300,300"), "double tap on: one tap alone comes after its wait");

    Rig d;
    d.g.setDoubleTapZoom(true);
    d.down(1, 300, 300);
    d.up(1, 0.05);
    d.down(1, 300, 300, 0.05);
    d.move(1, 360, 300);  // the second touch drags
    check(d.has("tap 300,300") && !d.has("double"), "double tap on: a tap then a drag is a tap and a drag");
    d.up(1);
}

}  // namespace

int main()
{
    slopAndTap();
    pan();
    pinch();
    quickPinchRelease();
    tapAndLongPress();
    flingAndCatch();
    clamps();
    list();
    wheelEases();
    doubleTap();
    fillBounds();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
