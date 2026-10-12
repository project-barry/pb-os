#!/usr/bin/env python3
"""Draw the PB-OS boot splash (Plymouth script theme).

The Project Barry cat (cat.png, 1 px per art pixel, from the PB-OS Installer
icon) on its red square, with "PB-OS" in purple pixel letters below. A 3 s
loop: the tail tip swings out and back, and he blinks once.

Two scripts share the pictures: pb-os.script is just the loop;
pb-os-wave.script first plays an intro once (he lifts his right front paw
and waves), then the loop. PB-OS alternates them from boot to boot (theme
pb-os-wave points here, pbos-splash-next switches).

Each distinct picture of the square is written once (tile-NN.png); the
scripts play them from a frame table at 30 frames per second.

Usage: make-theme.py [--gif LOOP.gif] [--gif-wave WAVE.gif] <theme dir>
(needs Pillow)
"""
import math
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
FPS = 30
LOOP = 90            # 3 s: one tail swing, one blink
SCALE = 13           # screen pixels per art pixel (Plymouth runs at DeviceScale=1)
TILE = 30            # the red square, in art pixels
CAT_AT = (0, 5)      # where cat.png sits on the square
RED = (250, 56, 56)
BLACK = (0, 0, 0)
BODY = (51, 51, 42)
YELLOW = (190, 183, 6)
WHITE = (255, 255, 255)
PURPLE = (168, 85, 247)
GAP = 4              # art pixels between the square and the text
TEXT_SCALE = 10

# The tail above the hip: cat.png row -> first tail column (the launcher ends
# just left of it). Each row slides sideways, the tip most, so the tail
# bends. Only outward: swung left, the tip hides behind the launcher.
TAIL = {5: 26, 6: 26, 7: 25, 8: 24, 9: 23, 10: 23, 11: 22, 12: 22}
TAIL_TOP, TAIL_BOTTOM = min(TAIL), max(TAIL)
WAG = 1.4            # art pixels at the tip (rounded per row)
BLINK = range(45, 50)  # loop frames with the eyes shut (about 1/6 s)

# Square coordinates (art pixels).
EYES = [(11, 9), (15, 9)]
PINK = (240, 150, 170)
LEG_COLORS = {"#": BLACK, "c": BODY, "P": PINK, "r": RED}
# His right front leg (screen left) lifted to wave: drawn over the square
# from (left, top); "." leaves the picture as it is, "r" is the red square
# where the standing leg was.
LEG = {
    "half": ((6, 15), ["........",
                       ".###....",
                       "#cPc#...",
                       "#ccc#...",
                       ".###r##.",
                       "....rrr.",
                       ".....rr."]),
    "up": ((5, 12), [".###.....",
                     "#ccc#....",
                     "#cPc#....",
                     ".##cc#...",
                     "...#cc...",
                     "....#c...",
                     "....##...",
                     ".....r##.",
                     ".....rrr.",
                     "......rr."]),
    "out": ((4, 11), [".###......",
                      "#ccc#.....",
                      "#cPc#.....",
                      ".##cc#....",
                      "...#cc#...",
                      "....#cc...",
                      ".....#c...",
                      ".....##...",
                      "......r##.",
                      "......rrr.",
                      ".......rr."]),
}

GLYPHS = {
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "-": ["...", "...", "...", "###", "...", "...", "..."],
}


def word(text):
    cols = sum(len(GLYPHS[c][0]) for c in text) + len(text) - 1
    img = Image.new("L", (cols, 7), 0)
    x = 0
    for c in text:
        for y, row in enumerate(GLYPHS[c]):
            for dx, bit in enumerate(row):
                if bit == "#":
                    img.putpixel((x + dx, y), 255)
        x += len(GLYPHS[c][0]) + 1
    return img


def split_tail(cat):
    """cat.png without the upper tail, and the tail rows on their own."""
    body = cat.copy()
    tail = {}
    for y, left in TAIL.items():
        for x in range(left, cat.width):
            p = cat.getpixel((x, y))
            if p[3]:
                tail[(x, y)] = p
                body.putpixel((x, y), (0, 0, 0, 0))
    return body, tail


def lift_leg(img, pose):
    (x0, y0), rows = LEG[pose]
    for dy, row in enumerate(rows):
        for dx, ch in enumerate(row):
            if ch in LEG_COLORS:
                img.putpixel((x0 + dx, y0 + dy), LEG_COLORS[ch] + (255,))


def bend(swing):
    """Whole-pixel shift of each tail row (row: dx) for a swing of 0..1."""
    return tuple(round(WAG * swing * (TAIL_BOTTOM + 1 - y) / (TAIL_BOTTOM + 1 - TAIL_TOP))
                 for y in sorted(TAIL))


def square(body, tail, bent=bend(0), eyes="open", paw=None):
    img = Image.new("RGBA", (TILE, TILE), RED + (255,))
    shift = dict(zip(sorted(TAIL), bent))
    # Tail first: the cat and its launcher stay in front of it.
    for (x, y), p in tail.items():
        tx, ty = CAT_AT[0] + x + shift[y], CAT_AT[1] + y
        if 0 <= tx < TILE:
            img.putpixel((tx, ty), p)
    img.alpha_composite(body, CAT_AT)
    if eyes == "shut":
        for p in EYES:
            img.putpixel(p, BLACK + (255,))
    if paw:
        lift_leg(img, paw)
    return img.resize((TILE * SCALE, TILE * SCALE), Image.NEAREST)


def loop_states():
    states = []
    for i in range(LOOP):
        swing = (1 - math.cos(2 * math.pi * i / LOOP)) / 2   # 0 → 1 → 0
        states.append(dict(bent=bend(swing), eyes="shut" if i in BLINK else "open"))
    return states


def intro_states():
    """Once, before the loop: a wave, then settle."""
    t = []
    t += [dict()] * 48                                   # 1.6 s: as usual
    t += [dict(paw="half")] * 3                          # the paw comes up
    for _ in range(3):                                   # three waves
        t += [dict(paw="up")] * 5
        t += [dict(paw="out")] * 5
    t += [dict(paw="up")] * 5
    t += [dict(paw="half")] * 3                          # and back down
    t += [dict()] * 15                                   # 0.5 s, then the tail starts
    return t


def script(intro, loop, tiles, size, words):
    """A Plymouth script: the intro once, then the loop, at FPS."""
    height = size + GAP * SCALE + words.height
    lines = [
        "# PB-OS boot splash. Written by external-and-mods/plymouth/theme-src/",
        "# make-theme.py: edit that, not this.",
        "Window.SetBackgroundTopColor(0, 0, 0);",
        "Window.SetBackgroundBottomColor(0, 0, 0);",
        f"Plymouth.SetRefreshRate({FPS});",
        "",
    ]
    used = sorted(set(intro + loop))
    lines += [f'tiles[{n}] = Image("tile-{n:02d}.png");' for n in used]
    lines += [f"intro[{n}] = {v};" for n, v in enumerate(intro)]
    lines += [f"loop[{n}] = {v};" for n, v in enumerate(loop)]
    lines += [
        "",
        f"top = Math.Int(Window.GetY() + Window.GetHeight() / 2 - {height // 2});",
        f"cat = Sprite(tiles[{loop[0] if not intro else intro[0]}]);",
        f"cat.SetPosition(Math.Int(Window.GetX() + Window.GetWidth() / 2 - {size // 2}), top, 1);",
        'words = Sprite(Image("words.png"));',
        f"words.SetPosition(Math.Int(Window.GetX() + Window.GetWidth() / 2 - {words.width // 2}), "
        f"top + {size + GAP * SCALE}, 1);",
        "",
        "tick = 0;",
        f"shown = {loop[0] if not intro else intro[0]};",
        "fun refresh_callback ()",
        "  {",
        f"    if (tick < {len(intro)})",
        "      i = intro[tick];",
        "    else",
        f"      i = loop[(tick - {len(intro)}) % {LOOP}];",
        "    if (i != shown)",
        "      {",
        "        cat.SetImage(tiles[i]);",
        "        global.shown = i;",
        "      }",
        "    global.tick = tick + 1;",
        "  }",
        "Plymouth.SetRefreshFunction(refresh_callback);",
    ]
    return "\n".join(lines) + "\n"


def preview(path, frames, tiles, size, words):
    """An animated GIF of the frames, on black like the panel."""
    canvas = (max(size, words.width), size + GAP * SCALE + words.height)
    images = []
    for i in frames:
        f = Image.new("RGB", canvas, BLACK)
        f.paste(tiles[i], ((canvas[0] - size) // 2, 0))
        f.paste(words, ((canvas[0] - words.width) // 2, size + GAP * SCALE), words)
        images.append(f)
    images[0].save(path, save_all=True, append_images=images[1:],
                   duration=1000 // FPS, loop=0, optimize=True)


def main():
    args = sys.argv[1:]
    gifs = {}
    for opt in ("--gif", "--gif-wave"):
        if opt in args:
            gifs[opt] = Path(args[args.index(opt) + 1])
            del args[args.index(opt):args.index(opt) + 2]
    out = Path(args[0])
    out.mkdir(parents=True, exist_ok=True)
    for old in list(out.glob("tile-*.png")) + list(out.glob("throbber-*.png")):
        old.unlink()

    body, tail = split_tail(Image.open(HERE / "cat.png").convert("RGBA"))
    keys, tiles = {}, []

    def index(state):
        key = tuple(sorted((k, v) for k, v in state.items()))
        if key not in keys:
            keys[key] = len(tiles)
            tiles.append(square(body, tail, **state))
        return keys[key]

    loop = [index(s) for s in loop_states()]
    intro = [index(s) for s in intro_states()]
    for n, img in enumerate(tiles):
        img.save(out / f"tile-{n:02d}.png", optimize=True)

    text = word("PB-OS")
    text = text.resize((text.width * TEXT_SCALE, text.height * TEXT_SCALE), Image.NEAREST)
    words = Image.new("RGBA", text.size, PURPLE + (0,))
    words.putalpha(text)
    words.save(out / "words.png", optimize=True)

    size = TILE * SCALE
    (out / "pb-os.script").write_text(script([], loop, tiles, size, words))
    (out / "pb-os-wave.script").write_text(script(intro, loop, tiles, size, words))
    if "--gif" in gifs:
        preview(gifs["--gif"], loop * 3, tiles, size, words)
    if "--gif-wave" in gifs:
        preview(gifs["--gif-wave"], intro + loop * 2, tiles, size, words)
    print(f"{len(tiles)} tiles; pb-os: loop {len(loop)} frames; "
          f"pb-os-wave: intro {len(intro)} + loop")


if __name__ == "__main__":
    main()
