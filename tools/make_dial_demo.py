# Draws the demo gauge art used by the example_dial_pngs layout: tach face, a fill ring, a needle, a vertical bar.
# usage: python make_dial_demo.py <output folder>      (PIL needed; the layout just uses the PNGs, so your own art replaces them)
import sys, math, os
from PIL import Image, ImageDraw, ImageFont
out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
S = 3                                   # draw at 3x, shrink for smooth edges
def canvas(w, h): return Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
def save(img, name, w, h): img.resize((w, h), Image.LANCZOS).save(os.path.join(out, name))
def pt(cx, cy, r, ang): a = math.radians(ang); return (cx + r * math.cos(a), cy + r * math.sin(a))
FROM, SWEEP = 135, 270                  # 0 rpm at 7:30, 10 (x1000) at 4:30: the angles the layout's dial and needle use

# ---- tach face 300x300: disc, rim, ticks and numbers 0-10 (nothing moves, so no ring or needle in it)
im = canvas(300, 300); d = ImageDraw.Draw(im); c = 150 * S
d.ellipse((2 * S, 2 * S, 298 * S, 298 * S), fill=(201, 206, 216, 255))            # chrome rim
d.ellipse((8 * S, 8 * S, 292 * S, 292 * S), fill=(18, 21, 27, 255))
d.ellipse((20 * S, 20 * S, 280 * S, 280 * S), outline=(43, 48, 59, 255), width=2 * S)
font = ImageFont.truetype("arialbd.ttf", 26 * S)
for k in range(0, 21):                                          # every half thousand
    v = k / 2; ang = FROM + SWEEP * v / 10; major = k % 2 == 0
    col = (235, 70, 60, 255) if v >= 8 else (236, 239, 244, 255)
    r1, r2 = (96, 116) if major else (104, 116)
    d.line([pt(c, c, r1 * S, ang), pt(c, c, r2 * S, ang)], fill=col, width=(4 if major else 2) * S)
    if major:
        x, y = pt(c, c, 74 * S, ang); t = str(int(v))
        w, h = d.textbbox((0, 0), t, font=font)[2:]
        d.text((x - w / 2, y - h / 2 - 3 * S), t, font=font, fill=col)
small = ImageFont.truetype("arialbd.ttf", 15 * S)
t = "RPM x1000"; w = d.textlength(t, font=small)
d.text((c - w / 2, c - 52 * S), t, font=small, fill=(134, 143, 160, 255))
save(im, "dial_face.png", 300, 300)

# ---- the ring: empty = the dim band, full = the same band in colour (orange into red). The dial element shows the full one
# only as far as the value; both are drawn the size of the face so they line up with it.
def ring(full):
    im = canvas(300, 300); d = ImageDraw.Draw(im)
    for i in range(0, 270):
        t = i / 270
        col = (int(255 - 20 * t), int(154 - 94 * t), int(30 + 10 * t), 255) if full else (43, 48, 59, 255)
        d.arc((12 * S, 12 * S, 288 * S, 288 * S), FROM + i - 0.3, FROM + i + 1.3, fill=col, width=16 * S)
    return im
save(ring(False), "dial_ring_empty.png", 300, 300)
save(ring(True), "dial_ring_full.png", 300, 300)

# ---- needle 150x16, drawn pointing RIGHT (3 o'clock) with its pivot 20 px in from the left, so it has a short tail
im = canvas(150, 16); d = ImageDraw.Draw(im)
d.polygon([(0, 5 * S), (20 * S, 3 * S), (149 * S, 8 * S), (20 * S, 13 * S), (0, 11 * S)], fill=(235, 70, 60, 255))
d.line([(20 * S, 8 * S), (140 * S, 8 * S)], fill=(255, 190, 170, 255), width=S)
save(im, "dial_needle.png", 150, 16)

# ---- a vertical bar made of two pictures: empty = the dim slot, full = the lit bar (the bar shows it from the bottom up)
im = canvas(36, 240); d = ImageDraw.Draw(im)
d.rounded_rectangle((0, 0, 36 * S - 1, 240 * S - 1), radius=8 * S, fill=(30, 34, 44, 255), outline=(60, 66, 80, 255), width=2 * S)
save(im, "bar_v_empty.png", 36, 240)
im = canvas(36, 240); d = ImageDraw.Draw(im)
for y in range(240 * S):
    t = y / (240 * S)
    d.line([(0, y), (36 * S, y)], fill=(int(70 + 20 * t), int(220 - 110 * t), 90, 255))
mask = Image.new("L", im.size, 0); ImageDraw.Draw(mask).rounded_rectangle((0, 0, 36 * S - 1, 240 * S - 1), radius=8 * S, fill=255)
im.putalpha(mask)
save(im, "bar_v_full.png", 36, 240)
print("wrote the demo art to", out)
