# Steam library art for PB-OS Sound Check: Barry's head (the PB-OS Quick
# Access icon's pixels) on PB-OS's dark blue. Committed as art/*.png.
# Usage: python3 make-art.py <NotoSans-Bold.ttf from the rootfs> art
import sys
from PIL import Image, ImageDraw, ImageFont
font_path, out = sys.argv[1], sys.argv[2]
HEAD = [(0,3,1),(0,9,1),(1,2,3),(1,8,3),(2,2,9),(3,1,11),(4,1,3),(4,5,3),(4,9,3),(5,0,6),(5,7,6),(6,1,11),(7,0,13),(8,2,9),(9,3,7),(10,4,5)]
BG_TOP, BG_BOT = (0x1c,0x24,0x38), (0x0e,0x12,0x1c)
WHITE, BLUE = (0xf2,0xf4,0xf8), (0x1a,0x9f,0xff)
def bg(w,h):
    im = Image.new('RGB',(w,h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y/(h-1)
        d.line([(0,y),(w,y)], fill=tuple(round(a+(b-a)*t) for a,b in zip(BG_TOP,BG_BOT)))
    return im
def head(d, x, y, px, color=WHITE):
    for r, c, wd in HEAD:
        d.rectangle([x+c*px, y+r*px, x+(c+wd)*px-1, y+(r+1)*px-1], fill=color)
def wave(d, cx, cy, s, color=BLUE):
    # three sound arcs right of the head
    for i in range(3):
        r = s*(i+1)
        d.arc([cx-r, cy-r, cx+r, cy+r], -45, 45, fill=color, width=max(2, s//3))
def text(d, xy, s, size, anchor='mm', color=WHITE):
    d.text(xy, s, font=ImageFont.truetype(font_path, size), fill=color, anchor=anchor)
# portrait 600x900
im = bg(600,900); d = ImageDraw.Draw(im); px = 22
head(d, (600-13*px)//2 - 30, 230, px); wave(d, (600+13*px)//2 - 10, 230+5*px, 26)
text(d, (300,600), 'PB-OS', 76); text(d, (300,690), 'Sound Check', 64, color=BLUE)
im.save(f'{out}/portrait.png')
# wide capsule 920x430
im = bg(920,430); d = ImageDraw.Draw(im); px = 16
head(d, 50, (430-11*px)//2, px); wave(d, 50+13*px+14, 430//2, 18)
text(d, (380,175), 'PB-OS', 76, 'lm'); text(d, (380,265), 'Sound Check', 64, 'lm', BLUE)
im.save(f'{out}/wide.png')
# hero 1920x620 (no text: Steam draws the logo over it)
im = bg(1920,620); d = ImageDraw.Draw(im); px = 30
head(d, 1920-13*px-260, (620-11*px)//2, px, (0x2a,0x33,0x48)); wave(d, 1920-230, 620//2, 34, (0x18,0x4a,0x7a))
im.save(f'{out}/hero.png')
# logo 900x300, transparent
im = Image.new('RGBA',(900,300),(0,0,0,0)); d = ImageDraw.Draw(im); px = 14
head(d, 20, (300-11*px)//2, px); text(d, (240,115), 'PB-OS', 84, 'lm'); text(d, (240,205), 'Sound Check', 72, 'lm', BLUE)
im.save(f'{out}/logo.png')
# icon 256x256
im = Image.new('RGBA',(256,256),(0,0,0,0)); d = ImageDraw.Draw(im)
d.rounded_rectangle([0,0,255,255], 40, fill=BG_TOP); px = 16
head(d, (256-13*px)//2, (256-11*px)//2, px)
im.save(f'{out}/icon.png')
