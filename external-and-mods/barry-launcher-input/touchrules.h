// Barry Launcher's touch rules: the numbers every touch surface in Barry.Input
// goes by (the trackpads' TrackpadGesture, EdgeSwipe, PanZoomGesture and the
// QML items on it), so a tap, a drag and a throw feel the same everywhere in
// the Launcher and in every Barry app. A touch that feels wrong is fixed
// here (or in the core that reads these), never in an app's QML.
//
// Distances are in design pixels (the 1240 x 1080 layout at s = 1; an item
// multiplies them by its s), times in seconds, speeds in design pixels a
// second.
#pragma once

namespace barry::touch {

// How far a finger may wander and still be a tap (or a long press); past it,
// a touch is a drag. Android's touch slop, in the Launcher's units.
constexpr double SLOP = 14;
// A tap waits this long for a second touch: the trackpad's tap-then-drag,
// a double tap.
constexpr double TAP_S = 0.22;
// The trackpad's tap: down no longer than this.
constexpr double TAP_MAX_S = 0.3;
// A finger held this long without moving is a long press (the Launcher's
// tiles).
constexpr double LONG_PRESS_S = 0.5;
// A double tap's second tap lands within this of the first.
constexpr double DOUBLE_TAP_SLOP = 100;

// A finger's speed at release: from its last moments (least squares over
// this window); a finger that rested this long before lifting threw
// nothing.
constexpr double VELOCITY_WINDOW_S = 0.1;
constexpr double STOPPED_S = 0.04;
// Throws slower than this do not move on; faster ones count as this fast
// (Android's 50 and 8000 dp/s).
constexpr double FLING_MIN = 130;
constexpr double FLING_MAX = 21000;
// A throw's glide: Android's scroller (OverScroller's spline), its friction
// and the screen's density it is figured for (design pixels per inch).
constexpr double FLING_FRICTION = 0.015;
constexpr double DESIGN_PPI = 420;
// A pinch whose zoom changed by more than this (as a ratio, over the
// velocity window) as it lifted was zooming: it throws nothing.
constexpr double PINCH_STILL = 0.02;

// A wheel notch (or the trackpad's two-finger scroll): this many design
// pixels, or this zoom factor.
constexpr double WHEEL_PX = 70;
constexpr double WHEEL_ZOOM = 1.25;
// A wheel notch eases over this long (the view is moving meanwhile).
constexpr double WHEEL_ANIM_S = 0.15;
// A double tap that zooms: by this factor, eased over this long.
constexpr double DOUBLE_TAP_ZOOM = 2;
constexpr double ZOOM_ANIM_S = 0.25;

// A swipe up from the bottom edge (EdgeSwipe, on a device without a Barry
// button): one finger, coming down this close to the bottom, lifted at
// least this far up, within this time (shelld's numbers on the Thor).
// Screen pixels.
constexpr double BOTTOM_SWIPE_EDGE = 45;
constexpr double BOTTOM_SWIPE_MIN = 150;
constexpr double BOTTOM_SWIPE_S = 0.8;

// Whether one finger from startY to endY (screen pixels down from the top)
// in this many seconds, on a screen this high, was a swipe up from the
// bottom edge.
constexpr bool bottomSwipe(double startY, double endY, double seconds, double height)
{
    return startY >= height - BOTTOM_SWIPE_EDGE && startY - endY >= BOTTOM_SWIPE_MIN && seconds <= BOTTOM_SWIPE_S;
}

}  // namespace barry::touch
