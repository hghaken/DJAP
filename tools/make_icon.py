# Draws the DJAP icon (Pillow): on a dark brushed-metal tile a round dial with a compressor curve - the
# input in amber going straight up, the output in teal bending over at the knee towards the ceiling - and three
# level bars rising at the top right. Small sizes (16 .. 32 px, the tray) get a bolder drawing without the bars.
# Writes assets/DJAP.ico (16 .. 256 px), assets/icon-256.png, the About window's wide logo assets/logo.png
# and the setup's header images installer/wizard-small-*.bmp.   Usage: python -I tools/make_icon.py
import io
import os
import random
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 1024  # drawing size; every size is scaled down from it

TEAL = (38, 196, 168)
AMBER = (240, 160, 48)
RING = (205, 208, 214)


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def tile(size, radius, top, bottom, seed):
    """Rounded square: vertical gradient with fine horizontal brush strokes, a light top edge and a dark rim."""
    im = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    face = Image.new('RGBA', (size, size))
    d = ImageDraw.Draw(face)
    rnd = random.Random(seed)
    for y in range(size):
        k = rnd.randint(-5, 5)
        c = mix(top, bottom, y / (size - 1))
        d.line([(0, y), (size, y)], fill=tuple(max(0, min(255, v + k)) for v in c) + (255,))
    mask = Image.new('L', (size, size), 0)
    m = ImageDraw.Draw(mask)
    pad = size * 0.04
    m.rounded_rectangle([pad, pad, size - pad, size - pad], radius=radius, fill=255)
    im.paste(face, (0, 0), mask)
    # sheen: the upper part a little lighter, fading out downwards
    sheen = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    s = ImageDraw.Draw(sheen)
    for y in range(int(size * 0.45)):
        s.line([(0, y), (size, y)], fill=(255, 255, 255, int(26 * (1 - y / (size * 0.45)))))
    clipped = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    clipped.paste(sheen, (0, 0), mask)
    im.alpha_composite(clipped)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([pad, pad, size - pad, size - pad], radius=radius, outline=(8, 8, 10, 255),
                        width=max(2, size // 110))
    return im


def dial(im, cx, cy, r, ring_width):
    """A dark round dial with a soft shadow and a light ring."""
    shadow = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).ellipse([cx - r, cy - r + r * 0.06, cx + r, cy + r + r * 0.06], fill=(0, 0, 0, 150))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(r * 0.06)))
    d = ImageDraw.Draw(im)
    for i in range(int(r), 0, -2):  # dark centre, a little lighter towards the rim
        t = i / r
        d.ellipse([cx - i, cy - i, cx + i, cy + i], fill=mix((10, 11, 13), (40, 42, 47), t ** 3) + (255,))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=RING + (255,), width=ring_width)


def stroke(d, pts, colour, width):
    """A polyline with round joints and round ends."""
    d.line(pts, fill=colour + (255,), width=width, joint='curve')
    for x, y in (pts[0], pts[-1]):
        d.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=colour + (255,))


def knee_curve(x0, y0, knee_x, top_y, x1, n=64):
    """Output of a compressor: 1:1 up to the knee, then a soft bend into a nearly flat line ending at top_y."""
    pts = []
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        if x <= knee_x:
            y = y0 - (x - x0)
        else:
            u = (x - knee_x) / (x1 - knee_x)
            yk = y0 - (knee_x - x0)
            y = yk - (yk - top_y) * (1 - (1 - u) ** 2.2)
        pts.append((x, y))
    return pts


def draw(bold):
    im = tile(S, S * 0.19, (78, 81, 88), (26, 27, 31), seed=11)
    cx, cy, r = (512, 512, 360) if bold else (468, 556, 322)
    dial(im, cx, cy, r, 40 if bold else 22)
    d = ImageDraw.Draw(im)
    w_out, w_in = (96, 64) if bold else (40, 26)
    x0, y0 = cx - r * 0.66, cy + r * 0.50
    if not bold:  # baseline
        d.line([(cx - r * 0.72, cy + r * 0.62), (cx + r * 0.72, cy + r * 0.62)], fill=(150, 154, 160, 255), width=8)
    # input (1:1) in amber, under the output
    stroke(d, [(x0, y0), (cx + r * 0.38, cy - r * 0.54)], AMBER, w_in)
    # output: bent over at the knee towards the ceiling, in teal
    stroke(d, knee_curve(x0, y0, cx - r * 0.30, cy - r * 0.22, cx + r * 0.70), TEAL, w_out)
    if not bold:  # three level bars at the top right, the last one in amber
        for i, (h, c) in enumerate([(70, TEAL), (125, TEAL), (180, AMBER)]):
            x = 790 + i * 62
            d.rounded_rectangle([x, 300 - h, x + 40, 300], radius=12, fill=mix(c, (255, 255, 255), 0.08) + (255,))
    return im


def banner(icon):
    """The wide logo of the About window (4:1): a light brushed plate with the icon and the name - "DJ" dark, "A" in
    teal, "P" in amber, engraved - over the name written out."""
    w, h = 1600, 400
    plate = Image.new('RGBA', (w, h))
    d = ImageDraw.Draw(plate)
    rnd = random.Random(5)
    for y in range(h):
        k = rnd.randint(-5, 5)
        c = mix((178, 176, 171), (124, 122, 117), y / (h - 1))  # the metal of the program's windows
        d.line([(0, y), (w, y)], fill=tuple(max(0, min(255, v + k)) for v in c) + (255,))
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=60, fill=255)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.paste(plate, (0, 0), mask)
    d = ImageDraw.Draw(out)
    d.rounded_rectangle([3, 3, w - 4, h - 4], radius=58, outline=(228, 226, 222, 150), width=5)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=60, outline=(58, 60, 64, 255), width=4)
    out.alpha_composite(icon.resize((340, 340), Image.LANCZOS), (30, 30))
    parts = (('DJ', (30, 32, 36)), ('A', (14, 150, 134)), ('P', (214, 132, 14)))
    size = 200
    while True:  # the largest bold size that fits beside the icon
        font = ImageFont.truetype(r'C:\Windows\Fonts\segoeuib.ttf', size)
        if sum(d.textlength(t, font=font) for t, _ in parts) <= w - 420 - 50:
            break
        size -= 4
    x, y = 420, 150 - size * 0.62
    for text, col in parts:
        d.text((x + 2, y + 3), text, font=font, fill=(255, 255, 255, 80))  # engraved highlight
        d.text((x, y), text, font=font, fill=col + (255,))
        x += d.textlength(text, font=font)
    small = ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf', 58)
    d.text((428, 262), 'DJ Audio Processor', font=small, fill=(40, 42, 47, 255))
    return out


def write_ico(path, images):
    """An .ico with 32-bit bitmaps (and AND masks) for the sizes below 256 and PNG for 256: older parts of Windows
    do not read PNG entries in small sizes."""
    entries = []
    for im in images:
        w, h = im.size
        if w >= 256:
            buf = io.BytesIO()
            im.save(buf, 'PNG')
            data = buf.getvalue()
        else:
            px = im.convert('RGBA').load()
            rows = []
            for y in range(h - 1, -1, -1):  # bottom-up, BGRA
                rows.append(b''.join(struct.pack('BBBB', *(px[x, y][2], px[x, y][1], px[x, y][0], px[x, y][3]))
                                     for x in range(w)))
            stride = ((w + 31) // 32) * 4
            mask = []
            for y in range(h - 1, -1, -1):  # 1 = transparent
                bits = bytearray(stride)
                for x in range(w):
                    if px[x, y][3] == 0:
                        bits[x // 8] |= 0x80 >> (x % 8)
                mask.append(bytes(bits))
            header = struct.pack('<IiiHHIIiiII', 40, w, 2 * h, 1, 32, 0, 0, 0, 0, 0, 0)
            data = header + b''.join(rows) + b''.join(mask)
        entries.append((w, h, data))
    out = struct.pack('<HHH', 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for w, h, data in entries:
        out += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(out + b''.join(d for _, _, d in entries))


def main():
    big = draw(bold=False)
    small = draw(bold=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    images = [(small if s <= 32 else big).resize((s, s), Image.LANCZOS) for s in sizes]
    os.makedirs(os.path.join(ROOT, 'assets'), exist_ok=True)
    write_ico(os.path.join(ROOT, 'assets', 'DJAP.ico'), images)
    images[-1].save(os.path.join(ROOT, 'assets', 'icon-256.png'))
    banner(big).save(os.path.join(ROOT, 'assets', 'logo.png'))
    # setup header images (100 % .. 200 %), on the wizard's white header
    for s in (55, 64, 83, 92, 110):
        bmp = Image.new('RGB', (s, s), (255, 255, 255))
        bmp.paste(big.resize((s, s), Image.LANCZOS), (0, 0), big.resize((s, s), Image.LANCZOS))
        bmp.save(os.path.join(ROOT, 'installer', f'wizard-small-{s}.bmp'))
    print('assets/DJAP.ico, assets/icon-256.png, installer/wizard-small-*.bmp')


if __name__ == '__main__':
    main()
