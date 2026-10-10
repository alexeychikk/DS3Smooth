# Draws keyboard/mouse versions of DS3's 32x32 Xbox button icons and encodes them for DS3MouseFix.
#
# DS3 on PC only has gamepad icons. Each one is a separate 32x32 BC7 texture; DS3MouseFix
# (dinput8.dll) recognises the original by a hash of its pixels and creates ours instead. The hashes
# below were found by dumping the textures the game creates (dump_textures = 1 in DS3Smooth.ini).
#
#   python make_icons.py <out dir>        (needs Pillow, and ..\tools\texconv.exe for BC7)
#
# Labels come from ICONS below: plain text draws a key cap; "LMB", "RMB", "MMB", "Mouse" draw a mouse;
# "arrows:UD" draws the four arrow keys with the listed ones lit; "shift:L" draws Shift+Left etc.

import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

# Original texture hash -> what the button does on keyboard/mouse, from the user's DS3 key config
# (Settings > Key bindings). Prompts are mostly shown in menus, so where a button does different things
# in menus and in gameplay, the menu key wins (e.g. B is Cancel = Q in menus, roll = Space in gameplay).
# The same icons fill the "Gamepad" column of the key config screen, which will show these too.
ICONS = {
    "c3a90c813af7cd90": "E",          # A: Confirm / Interact
    "cfa779b24b43beb2": "Q",          # B: Cancel (gameplay roll is Space)
    "537a066088061ae6": "R",          # X: Function 1 / Use item
    "46c06aed22cc2b11": "F",          # Y: Function 2 / Two-hand
    "789247282afe4db0": "shift:L",    # LB: previous tab (gameplay left attack is RMB)
    "645eda6eb7317eba": "shift:R",    # RB: next tab (gameplay right attack is LMB)
    "89848b677af9ab4d": "shift:U",    # LT: scroll up (gameplay left strong attack is mouse 4)
    "e55559021c87a43b": "shift:D",    # RT: scroll down (gameplay right strong attack is mouse 3)
    "cefb11341fa7864e": "X",          # L3 click: Sort (gameplay jump is Left Ctrl)
    "219dee7101bfe99a": "C",          # R3 click: Function 3 (gameplay lock-on is Q / wheel click)
    "124feaf734542843": "G",          # View: gestures / help
    "384a18cee82f4fb3": "Esc",        # Menu
    "724f2ffd91d02f6c": "WASD",       # left stick
    "d0ef2d846eb89a34": "WASD",       # left stick (outlined)
    "8796a12eda27a138": "Mouse",      # right stick: camera
    "47a9b91a45f98d33": "Mouse",      # right stick (outlined)
    "2aee73a07052ee56": "arrows:UDLR",  # D-pad
    "5c3d9ac9144dd9e4": "arrows:UDLR",  # D-pad, all four marked
    "4e543a487c48213d": "arrows:U",     # D-pad up: switch spell / cursor up
    "ee26509efb03911c": "arrows:D",     # D-pad down: switch item / cursor down
    "5affe1084d693e06": "arrows:R",     # D-pad right: switch right weapon
    "dfa24b94e56dde40": "arrows:UD",    # D-pad up/down
    "0546af78b0e56645": "arrows:LR",    # D-pad left/right
}

SIZE = 32
SCALE = 8  # draw at 256x256, then downsample for clean edges
HERE = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(HERE, "..", "tools", "texconv.exe")

# Colours taken from the game's own icons: dark bevelled body, worn gold rim and letters.
BODY_TOP = (78, 70, 62)
BODY_BOTTOM = (22, 19, 17)
RIM = (150, 128, 92, 255)
GOLD = (236, 204, 140, 255)
SHADOW = (0, 0, 0, 200)
UNLIT = (88, 76, 60, 255)
FONT_FILES = ["segoeuib.ttf", "seguisb.ttf", "arialbd.ttf"]


def gradient(s):
    """Vertical body gradient, lit from above like the game's icons."""
    g = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(g)
    for y in range(s):
        t = y / (s - 1)
        d.line([0, y, s, y], fill=tuple(int(a + (b - a) * t) for a, b in zip(BODY_TOP, BODY_BOTTOM)) + (255,))
    return g


def shape(s, box, radius):
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def font(px):
    for name in FONT_FILES:
        try:
            return ImageFont.truetype(os.path.join(os.environ["WINDIR"], "Fonts", name), px)
        except OSError:
            continue
    return ImageFont.load_default()


def fit_text(draw, text, max_w, max_h):
    px = max_h
    while px > 8:
        f = font(px)
        l, t, r, b = draw.textbbox((0, 0), text, font=f)
        if r - l <= max_w and b - t <= max_h:
            return f, (l, t, r, b)
        px -= 2
    f = font(px)
    return f, draw.textbbox((0, 0), text, font=f)


def blank_cap():
    s = SIZE * SCALE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    m, w = SCALE // 2, 2 * SCALE  # margin, rim width
    im.paste(Image.new("RGBA", (s, s), RIM), (0, 0), shape(s, [m, m, s - m, s - m], 8 * SCALE))
    im.paste(gradient(s), (0, 0), shape(s, [m + w, m + w, s - m - w, s - m - w], 6 * SCALE))
    return im


DIRECTIONS = {"U": (0, -1), "D": (0, 1), "L": (-1, 0), "R": (1, 0)}


def triangle(d, cx, cy, direction, r, fill):
    dx, dy = DIRECTIONS[direction]
    px, py = -dy, dx  # perpendicular
    tip = (cx + dx * r, cy + dy * r)
    base = [(cx - dx * r * 0.6 + px * r, cy - dy * r * 0.6 + py * r), (cx - dx * r * 0.6 - px * r, cy - dy * r * 0.6 - py * r)]
    d.polygon([tip] + base, fill=fill)


def arrow_keys(lit):
    """The four arrow keys as a cross of triangles; the ones in `lit` are gold, the rest dimmed."""
    im = blank_cap()
    d = ImageDraw.Draw(im)
    c, off, r = SIZE * SCALE / 2, 7.5 * SCALE, 4.2 * SCALE
    for k, (dx, dy) in DIRECTIONS.items():
        triangle(d, c + dx * off + SCALE * 0.6, c + dy * off + SCALE * 0.6, k, r, SHADOW)
        triangle(d, c + dx * off, c + dy * off, k, r, GOLD if k in lit else UNLIT)
    return im


def shift_arrow(direction):
    """Shift+arrow: the hollow Shift arrow on the left, the arrow key on the right."""
    im = blank_cap()
    d = ImageDraw.Draw(im)
    u = SCALE
    # Shift symbol: hollow upward arrow with a stem.
    cx, top = 10.5 * u, 8 * u
    outline = [(cx, top), (cx + 5.5 * u, top + 7 * u), (cx + 2.5 * u, top + 7 * u), (cx + 2.5 * u, top + 15 * u),
               (cx - 2.5 * u, top + 15 * u), (cx - 2.5 * u, top + 7 * u), (cx - 5.5 * u, top + 7 * u)]
    d.polygon([(x + 0.6 * u, y + 0.6 * u) for x, y in outline], outline=SHADOW, width=int(1.6 * u))
    d.polygon(outline, outline=GOLD, width=int(1.6 * u))
    triangle(d, 22.5 * u + 0.6 * u, 16 * u + 0.6 * u, direction, 5.2 * u, SHADOW)
    triangle(d, 22.5 * u, 16 * u, direction, 5.2 * u, GOLD)
    return im


def key_cap(text):
    s = SIZE * SCALE
    m, w = SCALE // 2, 2 * SCALE
    im = blank_cap()
    d = ImageDraw.Draw(im)
    inner = s - 2 * (m + w) - 3 * SCALE
    f, (l, t, r, b) = fit_text(d, text, inner, int(inner * (0.7 if len(text) == 1 else 0.46)))
    x, y = (s - (r - l)) / 2 - l, (s - (b - t)) / 2 - t
    d.text((x + SCALE, y + SCALE), text, font=f, fill=SHADOW)
    d.text((x, y), text, font=f, fill=GOLD)
    return im


def mouse(button):
    s = SIZE * SCALE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    x0, x1, y0, y1 = 6 * SCALE, 26 * SCALE, SCALE, 31 * SCALE
    mid, split, w = s // 2, 13 * SCALE, 2 * SCALE
    body = [x0 + w, y0 + w, x1 - w, y1 - w]
    im.paste(Image.new("RGBA", (s, s), RIM), (0, 0), shape(s, [x0, y0, x1, y1], 10 * SCALE))
    im.paste(gradient(s), (0, 0), shape(s, body, 8 * SCALE))
    # The pressed button is filled gold; the split lines stay visible either way.
    if button in ("LMB", "RMB"):
        half = Image.new("L", (s, s), 0)
        ImageDraw.Draw(half).rectangle([body[0], body[1], mid, split] if button == "LMB" else [mid, body[1], body[2], split], fill=255)
        mask = Image.composite(half, Image.new("L", (s, s), 0), shape(s, body, 8 * SCALE))
        im.paste(Image.new("RGBA", (s, s), GOLD), (0, 0), mask)
    d = ImageDraw.Draw(im)
    d.line([body[0], split, body[2], split], fill=RIM, width=w)
    d.line([mid, body[1], mid, split], fill=RIM, width=w)
    if button in ("MMB", "Wheel"):
        d.rounded_rectangle([mid - 3 * SCALE, 4 * SCALE, mid + 3 * SCALE, 12 * SCALE], radius=3 * SCALE, fill=GOLD)
    return im


def draw(label):
    if label in ("LMB", "RMB", "MMB", "Wheel", "Mouse"):
        big = mouse(label)
    elif label.startswith("arrows:"):
        big = arrow_keys(label[7:])
    elif label.startswith("shift:"):
        big = shift_arrow(label[6:])
    else:
        big = key_cap(label)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "DS3KeyIcons")
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        pngs = []
        for h, label in ICONS.items():
            path = os.path.join(tmp, h + ".png")
            draw(label).save(path)
            pngs.append(path)
        # BC7, one mip, DX10 header: the same as the game's originals.
        subprocess.run([TEXCONV, "-nologo", "-y", "-f", "BC7_UNORM", "-m", "1", "-dx10", "-bc", "x", "-o", out] + pngs,
                       check=True, stdout=subprocess.DEVNULL)
        preview = Image.new("RGBA", (len(pngs) * 40, 40), (60, 60, 70, 255))
        for i, p in enumerate(pngs):
            preview.alpha_composite(Image.open(p), (i * 40 + 4, 4))
        preview.resize((preview.width * 4, preview.height * 4), Image.NEAREST).save(os.path.join(HERE, "preview.png"))
    print("%d icons written to %s" % (len(ICONS), out))


if __name__ == "__main__":
    main()
