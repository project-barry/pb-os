// Line icons for the shell's buttons, drawn so they look the same whatever
// fonts are installed. kind: back, forward, reload, stop, home, close, globe,
// chat, trackpad, keyboard, app (a user app without an icon of its own),
// the keyboard keys, and the apps' logos as line
// art in the same style: firefox, discord, signal (outlines from Tabler
// Icons, MIT), and dino; the quick settings' wifi, bluetooth, camera,
// record, square, sun, moon and volume (Tabler too); all drawn on a 24-unit grid and scaled.
import QtQuick
import QtQuick.Shapes

Shape {
    id: icon
    property string kind
    property color color: "white"
    property real lineWidth: 6

    readonly property real w: width
    readonly property real h: height

    preferredRendererType: Shape.CurveRenderer

    readonly property string logoPath: ({
        firefox: "M4.028 7.82a9 9 0 1 0 12.823 -3.4c-1.636 -1.02 -3.064 -1.02 -4.851 -1.02h-1.647"
               + "M4.914 9.485c-1.756 -1.569 -.805 -5.38 .109 -6.17c.086 .896 .585 1.208 1.111 1.685"
               + "c.88 -.275 1.313 -.282 1.867 0c.82 -.91 1.694 -2.354 2.628 -2.093c-1.082 1.741 -.07 3.733 1.371 4.173"
               + "c-.17 .975 -1.484 1.913 -2.76 2.686c-1.296 .938 -.722 1.85 0 2.234c.949 .506 3.611 -1 4.545 .354"
               + "c-1.698 .102 -1.536 3.107 -3.983 2.727c2.523 .957 4.345 .462 5.458 -.34"
               + "c1.965 -1.52 2.879 -3.542 2.879 -5.557c-.014 -1.398 .194 -2.695 -1.26 -4.75",
        discord: "M8 12a1 1 0 1 0 2 0a1 1 0 0 0 -2 0"
               + "M14 12a1 1 0 1 0 2 0a1 1 0 0 0 -2 0"
               + "M15.5 17c0 1 1.5 3 2 3c1.5 0 2.833 -1.667 3.5 -3c.667 -1.667 .5 -5.833 -1.5 -11.5"
               + "c-1.457 -1.015 -3 -1.34 -4.5 -1.5l-.972 1.923a11.913 11.913 0 0 0 -4.053 0l-.975 -1.923"
               + "c-1.5 .16 -3.043 .485 -4.5 1.5c-2 5.667 -2.167 9.833 -1.5 11.5c.667 1.333 2 3 3.5 3c.5 0 2 -2 2 -3"
               + "M7 16.5c3.5 1 6.5 1 10 0",
        signal: "M2.708 22.726l.148 -.034l.822 -.191m2.754 -.642l.472 -.11c.485 .254 .987 .469 1.501 .647"
              + "m2.939 .584c.218 .013 .437 .02 .656 .02c.22 0 .439 -.008 .657 -.02m2.939 -.583"
              + "c.418 -.144 .824 -.312 1.217 -.504m2.484 -1.663c.329 -.292 .641 -.604 .933 -.933"
              + "m1.663 -2.485c.192 -.393 .36 -.799 .505 -1.216m.582 -2.94c.012 -.217 .02 -.436 .02 -.656"
              + "c0 -.22 -.008 -.439 -.02 -.657m-.583 -2.939c-.144 -.418 -.313 -.824 -.504 -1.217"
              + "m-1.663 -2.484c-.292 -.329 -.604 -.641 -.933 -.933m-2.485 -1.663c-.393 -.192 -.799 -.36 -1.216 -.505"
              + "m-2.94 -.582c-.217 -.012 -.436 -.02 -.656 -.02c-.22 0 -.439 .008 -.657 .02m-2.939 .583"
              + "c-.418 .144 -.824 .313 -1.217 .504m-2.484 1.663c-.329 .292 -.641 .604 -.933 .933"
              + "m-1.663 2.485c-.192 .393 -.36 .799 -.505 1.216m-.582 2.94c-.012 .217 -.02 .436 -.02 .656"
              + "c0 .219 .007 .438 .02 .657m.584 2.939c.178 .514 .394 1.016 .648 1.501l-.11 .471"
              + "m-.643 2.754l-.217 .93l-.009 .039c-.15 .643 .249 1.286 .892 1.436c.179 .041 .365 .041 .543 -.001"
              + "M4.848 19.152l2.446 -.57l.993 .519c.56 .292 1.167 .519 1.805 .675c.597 .149 1.238 .223 1.908 .223"
              + "c2.21 0 4.21 -.895 5.657 -2.342c1.447 -1.447 2.342 -3.447 2.342 -5.657c0 -2.21 -.895 -4.21 -2.342 -5.657"
              + "c-1.447 -1.447 -3.447 -2.342 -5.657 -2.342c-2.21 0 -4.21 .895 -5.657 2.342"
              + "c-1.447 1.447 -2.342 3.447 -2.342 5.657c0 .666 .075 1.308 .223 1.909c.156 .637 .383 1.244 .676 1.805"
              + "l.519 .993l-.571 2.445",
        // The quick settings' icons (Tabler Icons outlines).
        wifi: "M12 18h.01M9.172 15.172a4 4 0 0 1 5.656 0M6.343 12.343a8 8 0 0 1 11.314 0"
            + "M3.515 9.515c4.686 -4.687 12.284 -4.687 17 0",
        bluetooth: "M7 8l10 8l-5 4l0 -16l5 4l-10 8",
        camera: "M5 7h1a2 2 0 0 0 2 -2a1 1 0 0 1 1 -1h6a1 1 0 0 1 1 1a2 2 0 0 0 2 2h1a2 2 0 0 1 2 2v9"
              + "a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-9a2 2 0 0 1 2 -2M9 13a3 3 0 1 0 6 0a3 3 0 0 0 -6 0",
        square: "M7 7h10v10h-10z",
        record: "M5 12a7 7 0 1 0 14 0a7 7 0 1 0 -14 0M9.5 12a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0 -5 0",
        sun: "M8 12a4 4 0 1 0 8 0a4 4 0 1 0 -8 0M3 12h1m8 -9v1m8 8h1m-9 8v1"
           + "m-6.4 -15.4l.7 .7m12.1 -.7l-.7 .7m0 11.4l.7 .7m-12.1 -.7l-.7 .7",
        moon: "M12 3c.132 0 .263 0 .393 0a7.5 7.5 0 0 0 7.92 12.446a9 9 0 1 1 -8.313 -12.454z",
        volume: "M15 8a5 5 0 0 1 0 8M17.7 5a9 9 0 0 1 0 14M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2"
              + "l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5",
        // Head with an open jaw, back, tail, belly; legs, arm and eye apart.
        dino: "M15.5 9.5H19.5V8.5H16.5V7H21V4Q21 2.5 19.5 2.5H13.5Q12 2.5 12 4V10"
            + "L9 12L6 13.2L2 10.5L3 14.8Q6 17.5 9.5 17H13L15.5 13.5Z"
            + "M9.5 17V21.5H11M13 17V21.5H14.5M15.5 12.5H17.5V13.7",
    })[kind] || ""

    // Dots (keys, the chat's dots, the dino's eye) are filled circles as
    // wide as the line, where they were a near-zero line with round caps:
    // CurveRenderer drew stray triangles around such a stroke on the
    // Thor's GPU (green specks on the Keyboard tile, 2026-10-18).
    // Each: [x, y] as fractions of the icon.
    readonly property var dots: {
        switch (kind) {
        case "keyboard": {
            const d = []
            for (let r = 0; r < 2; r++)
                for (let c = 0; c < 5; c++)
                    d.push([0.2 + 0.15 * c, 0.37 + 0.12 * r])
            return d
        }
        case "chat":
            return [[0.32, 0.45], [0.5, 0.45], [0.68, 0.45]]
        case "dino":
            return [[14.5 / 24, 4.7 / 24]]
        }
        return []
    }
    function dotsPath(r) {
        return dots.map(([fx, fy]) => {
            const x = fx * w, y = fy * h
            return `M ${x - r} ${y} A ${r} ${r} 0 1 0 ${x + r} ${y} A ${r} ${r} 0 1 0 ${x - r} ${y} Z`
        }).join(" ")
    }

    // The logos, on their 24-unit grid; the line keeps the same width.
    Shape {
        visible: icon.logoPath !== ""
        width: 24
        height: 24
        transform: Scale { xScale: icon.w / 24; yScale: icon.h / 24 }
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: icon.color
            strokeWidth: icon.lineWidth * 24 / Math.max(1, icon.w)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.logoPath }
        }
    }

    ShapePath {
        strokeColor: icon.color
        strokeWidth: icon.lineWidth
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        joinStyle: ShapePath.RoundJoin
        PathSvg {
            path: {
                const w = icon.w, h = icon.h
                switch (icon.kind) {
                case "back":
                    return `M ${0.62 * w} ${0.2 * h} L ${0.32 * w} ${0.5 * h} L ${0.62 * w} ${0.8 * h}`
                case "forward":
                    return `M ${0.38 * w} ${0.2 * h} L ${0.68 * w} ${0.5 * h} L ${0.38 * w} ${0.8 * h}`
                case "reload":
                    // 300° arc with an arrowhead at its end
                    return `M ${0.8 * w} ${0.5 * h} A ${0.3 * w} ${0.3 * h} 0 1 1 ${0.65 * w} ${0.24 * h}`
                         + ` M ${0.66 * w} ${0.08 * h} L ${0.66 * w} ${0.25 * h} L ${0.49 * w} ${0.25 * h}`
                case "stop":
                case "close":
                    return `M ${0.25 * w} ${0.25 * h} L ${0.75 * w} ${0.75 * h} M ${0.75 * w} ${0.25 * h} L ${0.25 * w} ${0.75 * h}`
                case "app":
                    // four rounded tiles
                    return [[0.18, 0.18], [0.56, 0.18], [0.18, 0.56], [0.56, 0.56]].map(([x, y]) => {
                        const x0 = x * w, y0 = y * h, x1 = (x + 0.26) * w, y1 = (y + 0.26) * h, r = 0.05 * w
                        return `M ${x0 + r} ${y0} L ${x1 - r} ${y0} Q ${x1} ${y0} ${x1} ${y0 + r}`
                             + ` L ${x1} ${y1 - r} Q ${x1} ${y1} ${x1 - r} ${y1} L ${x0 + r} ${y1}`
                             + ` Q ${x0} ${y1} ${x0} ${y1 - r} L ${x0} ${y0 + r} Q ${x0} ${y0} ${x0 + r} ${y0} Z`
                    }).join(" ")
                case "home":
                    return `M ${0.15 * w} ${0.5 * h} L ${0.5 * w} ${0.18 * h} L ${0.85 * w} ${0.5 * h}`
                         + ` M ${0.25 * w} ${0.42 * h} L ${0.25 * w} ${0.82 * h} L ${0.75 * w} ${0.82 * h} L ${0.75 * w} ${0.42 * h}`
                case "shift":
                    return `M ${0.5 * w} ${0.12 * h} L ${0.12 * w} ${0.52 * h} L ${0.32 * w} ${0.52 * h}`
                         + ` L ${0.32 * w} ${0.85 * h} L ${0.68 * w} ${0.85 * h} L ${0.68 * w} ${0.52 * h}`
                         + ` L ${0.88 * w} ${0.52 * h} Z`
                case "backspace":
                    return `M ${0.3 * w} ${0.2 * h} L ${0.92 * w} ${0.2 * h} L ${0.92 * w} ${0.8 * h}`
                         + ` L ${0.3 * w} ${0.8 * h} L ${0.06 * w} ${0.5 * h} Z`
                         + ` M ${0.45 * w} ${0.36 * h} L ${0.73 * w} ${0.64 * h} M ${0.73 * w} ${0.36 * h} L ${0.45 * w} ${0.64 * h}`
                case "enter":
                    return `M ${0.85 * w} ${0.2 * h} L ${0.85 * w} ${0.6 * h} L ${0.15 * w} ${0.6 * h}`
                         + ` M ${0.35 * w} ${0.4 * h} L ${0.15 * w} ${0.6 * h} L ${0.35 * w} ${0.8 * h}`
                case "hide":
                    return `M ${0.2 * w} ${0.35 * h} L ${0.5 * w} ${0.65 * h} L ${0.8 * w} ${0.35 * h}`
                case "chat":
                    // speech bubble with three dots
                    return `M ${0.2 * w} ${0.18 * h} L ${0.8 * w} ${0.18 * h} Q ${0.92 * w} ${0.18 * h} ${0.92 * w} ${0.3 * h}`
                         + ` L ${0.92 * w} ${0.6 * h} Q ${0.92 * w} ${0.72 * h} ${0.8 * w} ${0.72 * h}`
                         + ` L ${0.42 * w} ${0.72 * h} L ${0.24 * w} ${0.88 * h} L ${0.26 * w} ${0.72 * h}`
                         + ` L ${0.2 * w} ${0.72 * h} Q ${0.08 * w} ${0.72 * h} ${0.08 * w} ${0.6 * h}`
                         + ` L ${0.08 * w} ${0.3 * h} Q ${0.08 * w} ${0.18 * h} ${0.2 * w} ${0.18 * h} Z`
                case "trackpad":
                    // pad with a split button row along its bottom
                    return `M ${0.12 * w} ${0.15 * h} L ${0.88 * w} ${0.15 * h} L ${0.88 * w} ${0.85 * h}`
                         + ` L ${0.12 * w} ${0.85 * h} Z`
                         + ` M ${0.12 * w} ${0.65 * h} L ${0.88 * w} ${0.65 * h}`
                         + ` M ${0.5 * w} ${0.65 * h} L ${0.5 * w} ${0.85 * h}`
                case "deck":
                    // a Steam Deck's body: its two square trackpads under its sticks
                    return `M ${0.16 * w} ${0.3 * h} L ${0.84 * w} ${0.3 * h} Q ${0.96 * w} ${0.3 * h} ${0.96 * w} ${0.42 * h}`
                         + ` L ${0.96 * w} ${0.62 * h} Q ${0.96 * w} ${0.74 * h} ${0.84 * w} ${0.74 * h}`
                         + ` L ${0.16 * w} ${0.74 * h} Q ${0.04 * w} ${0.74 * h} ${0.04 * w} ${0.62 * h}`
                         + ` L ${0.04 * w} ${0.42 * h} Q ${0.04 * w} ${0.3 * h} ${0.16 * w} ${0.3 * h} Z`
                         + ` M ${0.12 * w} ${0.5 * h} L ${0.26 * w} ${0.5 * h} L ${0.26 * w} ${0.66 * h} L ${0.12 * w} ${0.66 * h} Z`
                         + ` M ${0.74 * w} ${0.5 * h} L ${0.88 * w} ${0.5 * h} L ${0.88 * w} ${0.66 * h} L ${0.74 * w} ${0.66 * h} Z`
                         + ` M ${0.34 * w} ${0.38 * h} L ${0.66 * w} ${0.38 * h} L ${0.66 * w} ${0.66 * h} L ${0.34 * w} ${0.66 * h} Z`
                case "keyboard": {
                    // outline, two rows of keys, a space bar
                    let p = `M ${0.06 * w} ${0.25 * h} L ${0.94 * w} ${0.25 * h} L ${0.94 * w} ${0.75 * h}`
                          + ` L ${0.06 * w} ${0.75 * h} Z`
                          + ` M ${0.32 * w} ${0.62 * h} L ${0.68 * w} ${0.62 * h}`
                    return p
                }
                case "globe":
                    return `M ${0.05 * w} ${0.5 * h} A ${0.45 * w} ${0.45 * h} 0 1 1 ${0.95 * w} ${0.5 * h}`
                         + ` A ${0.45 * w} ${0.45 * h} 0 1 1 ${0.05 * w} ${0.5 * h}`
                         + ` M ${0.5 * w} ${0.05 * h} A ${0.2 * w} ${0.45 * h} 0 1 0 ${0.5 * w} ${0.95 * h}`
                         + ` A ${0.2 * w} ${0.45 * h} 0 1 0 ${0.5 * w} ${0.05 * h}`
                         + ` M ${0.05 * w} ${0.5 * h} L ${0.95 * w} ${0.5 * h}`
                         + ` M ${0.13 * w} ${0.27 * h} L ${0.87 * w} ${0.27 * h}`
                         + ` M ${0.13 * w} ${0.73 * h} L ${0.87 * w} ${0.73 * h}`
                }
                return ""
            }
        }
    }
    ShapePath {
        strokeColor: "transparent"
        fillColor: icon.dots.length ? icon.color : "transparent"
        PathSvg { path: icon.dotsPath(icon.lineWidth / 2) }
    }
}
