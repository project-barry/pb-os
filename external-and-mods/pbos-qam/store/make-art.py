#!/usr/bin/env python3
# The App Store's Steam artwork (store/art/ID/), made from each app's own
# logo (store/art-src/, sources and licenses in store/art-src/SOURCES.md).
# The frontend gives it to Steam with the app's shortcut (sections/store.js).
#
#   capsule.jpg  600x900   Steam's portrait cover (asset type 0)
#   wide.jpg     920x430   the wide cover (3)
#   hero.jpg     1920x620  the background at the top of the game page (1)
#   logo.png     the logo over the hero (2)
#   icon.png     256x256   the shortcut's icon (SetShortcutIcon), when there is a logo
#
# Two steps, because few machines have both tools:
#   1. Every SVG in art-src as a 1024 px PNG, e.g. on a PB-OS device:
#        for f in art-src/*.svg; do rsvg-convert -w 1024 -a "$f" -o "rendered/$(basename "${f%.svg}").png"; done
#      (PNG sources are used as they are.)
#   2. python3 make-art.py <rendered dir> [--font FILE] [ids...]   (needs Pillow)
# The committed art was made with Helvetica Neue Bold (macOS) as the font.
# Apps without a logo we may ship get their name only (NAME_ONLY).
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# Logos we do not ship: DuckStation's icon is icons8 art (its README), not
# under DuckStation's license; BigPEmu is closed freeware that may not be
# redistributed without its terms, and its icon is 128 px.
NAME_ONLY = {"duckstation": (120, 70, 20), "bigpemu": (120, 24, 30)}
# Logos to recolor for a dark background.
RECOLOR = {"retroarch": (255, 255, 255)}
# Square logos with their own background get rounded corners.
ROUND = {"chiaki"}
# Logos that are the name already: Steam's logo is the logo alone.
WORDMARK = {"mame"}


def load_font(path, size, index):
    try:
        return ImageFont.truetype(path, size, index=index)
    except OSError:
        return ImageFont.truetype(path, size)


def fit_font(draw, text, path, index, size, max_w):
    while size > 12:
        f = load_font(path, size, index)
        x0, _, x1, _ = draw.textbbox((0, 0), text, font=f)
        if x1 - x0 <= max_w:
            return f
        size -= 2
    return load_font(path, size, index)


def text(im, s, cx, cy, size, max_w, fnt, fill=(255, 255, 255)):
    d = ImageDraw.Draw(im)
    f = fit_font(d, s, fnt[0], fnt[1], size, max_w)
    x0, y0, x1, y1 = d.textbbox((0, 0), s, font=f)
    d.text((cx - (x1 - x0) / 2 - x0, cy - (y1 - y0) / 2 - y0), s, font=f, fill=fill)


def theme(logo, fallback):
    """Top and bottom background colors from the logo's own colors."""
    if logo is None:
        r, g, b = fallback
    else:
        small = logo.resize((64, 64)).load()
        data = [small[x, y] for x in range(64) for y in range(64)]
        px = [p for p in data if p[3] > 200 and max(p[:3]) - min(p[:3]) > 40]
        if not px:
            px = [p for p in data if p[3] > 200] or [(90, 90, 110, 255)]
        r, g, b = (sum(p[i] for p in px) / len(px) for i in range(3))
    peak = max(r, g, b, 1)
    top = tuple(int(c / peak * 70) for c in (r, g, b))
    bottom = tuple(int(c / peak * 22) for c in (r, g, b))
    return top, bottom


def background(w, h, top, bottom, logo, glow, cx, cy):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(h - 1, 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    if logo is not None:
        n = int(min(w, h) * 0.9)
        g = Image.new("RGBA", (n * 3, n * 3), (0, 0, 0, 0))
        lg = fit(logo, n, n)
        g.paste(lg, ((g.width - lg.width) // 2, (g.height - lg.height) // 2))
        g = g.filter(ImageFilter.GaussianBlur(n // 5))
        g.putalpha(g.getchannel("A").point(lambda v: int(v * glow)))
        im.paste(g, (int(cx - g.width / 2), int(cy - g.height / 2)), g)
    return im


def fit(logo, w, h):
    s = min(w / logo.width, h / logo.height)
    return logo.resize((max(1, int(logo.width * s)), max(1, int(logo.height * s))), Image.LANCZOS)


def put(im, logo, w, h, cx, cy):
    lg = fit(logo, w, h)
    im.paste(lg, (int(cx - lg.width / 2), int(cy - lg.height / 2)), lg)


def prepare(app_id, path):
    logo = Image.open(path).convert("RGBA")
    if logo.width < 1024 and logo.height < 1024:
        s = 1024 / max(logo.size)
        logo = logo.resize((int(logo.width * s), int(logo.height * s)), Image.LANCZOS)
    if app_id in RECOLOR:
        c = RECOLOR[app_id]
        solid = Image.new("RGBA", logo.size, c + (255,))
        solid.putalpha(logo.getchannel("A"))
        logo = solid
    if app_id in ROUND:
        m = Image.new("L", logo.size, 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, logo.width - 1, logo.height - 1], radius=logo.width // 5, fill=255)
        a = Image.composite(logo.getchannel("A"), Image.new("L", logo.size, 0), m)
        logo.putalpha(a)
    bbox = logo.getbbox()
    return logo.crop(bbox) if bbox else logo


def build(app, logo, out, fnt):
    name, sub = app["name"], app.get("summary", "")
    top, bottom = theme(logo, NAME_ONLY.get(app["id"], (90, 90, 110)))
    os.makedirs(out, exist_ok=True)
    jpg = dict(quality=88, optimize=True, progressive=True)

    p = background(600, 900, top, bottom, logo, 0.55, 300, 370)
    if logo is not None:
        put(p, logo, 400, 380, 300, 370)
        text(p, name, 300, 660, 92, 540, fnt)
        text(p, sub, 300, 740, 34, 540, fnt, (225, 225, 235))
    else:
        text(p, name, 300, 420, 110, 540, fnt)
        text(p, sub, 300, 520, 38, 540, fnt, (225, 225, 235))
    p.save(f"{out}/capsule.jpg", **jpg)

    w = background(920, 430, top, bottom, logo, 0.6, 230, 215)
    if logo is not None:
        put(w, logo, 280, 280, 230, 215)
        text(w, name, 640, 190, 104, 500, fnt)
        text(w, sub, 640, 275, 34, 500, fnt, (225, 225, 235))
    else:
        text(w, name, 460, 185, 120, 840, fnt)
        text(w, sub, 460, 285, 38, 840, fnt, (225, 225, 235))
    w.save(f"{out}/wide.jpg", **jpg)

    h = background(1920, 620, top, bottom, logo, 0.6, 1500, 310)
    if logo is not None:
        put(h, logo, 520, 420, 1500, 310)
    h.save(f"{out}/hero.jpg", **jpg)

    # The logo Steam lays over the hero: the logo and the name, transparent.
    L = Image.new("RGBA", (2400, 320), (0, 0, 0, 0))
    x = 20
    if logo is not None:
        lg = fit(logo, 560, 280)
        L.paste(lg, (x, 160 - lg.height // 2), lg)
        x += lg.width + 40
    if app["id"] not in WORDMARK:
        d = ImageDraw.Draw(L)
        f = load_font(fnt[0], 170, fnt[1])
        x0, y0, x1, y1 = d.textbbox((0, 0), name, font=f)
        d.text((x - x0, 160 - (y1 - y0) / 2 - y0), name, font=f, fill=(255, 255, 255))
    L = L.crop(L.getbbox())
    L.thumbnail((1280, 320), Image.LANCZOS)
    L.save(f"{out}/logo.png", optimize=True)

    if logo is not None:
        ic = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        put(ic, logo, 240, 240, 128, 128)
        ic.save(f"{out}/icon.png", optimize=True)
    elif os.path.exists(f"{out}/icon.png"):
        os.remove(f"{out}/icon.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rendered", help="1024 px PNGs of the art-src SVGs")
    ap.add_argument("ids", nargs="*")
    ap.add_argument("--font", default="/System/Library/Fonts/HelveticaNeue.ttc")
    ap.add_argument("--font-index", type=int, default=1)
    a = ap.parse_args()
    apps = json.load(open(f"{HERE}/catalog.json"))["apps"]
    for app in apps:
        i = app["id"]
        if a.ids and i not in a.ids:
            continue
        logo = None
        if i not in NAME_ONLY:
            src = next((p for p in (f"{a.rendered}/{i}.png", f"{HERE}/art-src/{i}.png") if os.path.exists(p)), None)
            if not src:
                sys.exit(f"no logo for {i} (render art-src/{i}.svg into {a.rendered})")
            logo = prepare(i, src)
        build(app, logo, f"{HERE}/art/{i}", (a.font, a.font_index))
        print("made", i)


if __name__ == "__main__":
    main()
