"""Generates res/qblank.ico.

The mark is a screen with a signal chevron inside it: two shapes, high
contrast, no thin strokes. That is what survives being drawn at 16x16 in a
taskbar, which is where this icon is seen most of the time. Deliberately not a
record dot -- qBlank does not record, and promising that in the icon would be
the first bug report.

Pure standard library, so there is no Pillow dependency to install. The small
sizes are uncompressed 32-bit BGRA, the large ones PNG: Windows reads PNG icon
images since Vista, and as bitmaps the three largest were 93 per cent of the
file -- 355 KB carried inside the executable for a flat two-shape mark.

    python tools/make_icon.py
"""

import math
import os
import struct
import zlib

ACCENT = (0x8B, 0x5C, 0xF6)      # violet, matching the default accent colour
ACCENT_BRIGHT = (0xC4, 0xA6, 0xFF)
SCREEN_BG = (0x1A, 0x14, 0x2E)   # near black with a violet cast

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
# From this size up the image goes into the icon as PNG. The sizes below stay
# bitmaps: they are what the taskbar, the tray and the title bar load, they are
# small either way, and a bitmap is the one form every icon API reads.
PNG_FROM = 64
SS = 4  # supersampling factor per axis


def lerp(a, b, t):
    return a + (b - a) * t


def blend(dst, src, alpha):
    """Source-over of an opaque colour onto an (r, g, b, a) pixel."""
    if alpha <= 0.0:
        return dst
    if alpha >= 1.0:
        return (src[0], src[1], src[2], 1.0)
    out_a = alpha + dst[3] * (1.0 - alpha)
    if out_a <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)
    r = (src[0] * alpha + dst[0] * dst[3] * (1.0 - alpha)) / out_a
    g = (src[1] * alpha + dst[1] * dst[3] * (1.0 - alpha)) / out_a
    b = (src[2] * alpha + dst[2] * dst[3] * (1.0 - alpha)) / out_a
    return (r, g, b, out_a)


def rounded_rect_contains(x, y, x0, y0, x1, y1, radius):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + radius), x1 - radius)
    cy = min(max(y, y0 + radius), y1 - radius)
    dx, dy = x - cx, y - cy
    return dx * dx + dy * dy <= radius * radius + 1e-9


def chevron_contains(x, y, cx, cy, half_h, thickness, lean):
    """A '>' shape: two arms meeting at the right, pointing forward.

    Signal moving to the right reads as "live feed" rather than "play button",
    which is what this program actually is.
    """
    dy = y - cy
    if abs(dy) > half_h:
        return False
    # x of the arm centreline at this height
    arm_x = cx + lean * (half_h - abs(dy))
    return abs(x - arm_x) <= thickness * 0.5


def render(size):
    """Returns a list of (r, g, b, a) floats, row-major, top-down."""
    n = size * SS
    px = [(0.0, 0.0, 0.0, 0.0)] * (n * n)

    # Layout in 0..1 space, then scaled. The screen is deliberately wide and
    # short so the silhouette is not confusable with a generic app square.
    margin_x = 0.085
    margin_y = 0.185
    x0, x1 = margin_x * n, (1.0 - margin_x) * n
    y0, y1 = margin_y * n, (1.0 - margin_y) * n
    radius = 0.11 * n
    border = max(1.6 * SS, 0.075 * n)

    cx = (x0 + x1) * 0.5
    cy = (y0 + y1) * 0.5
    half_h = (y1 - y0) * 0.26
    thickness = max(1.6 * SS, 0.085 * n)
    lean = 0.62

    for iy in range(n):
        y = iy + 0.5
        row = iy * n
        for ix in range(n):
            x = ix + 0.5
            p = (0.0, 0.0, 0.0, 0.0)

            outer = rounded_rect_contains(x, y, x0, y0, x1, y1, radius)
            if outer:
                inner = rounded_rect_contains(x, y, x0 + border, y0 + border, x1 - border,
                                              y1 - border, max(radius - border, 1.0))
                if inner:
                    # Subtle vertical gradient so the screen does not read flat.
                    t = (y - y0) / max(y1 - y0, 1.0)
                    shade = lerp(1.12, 0.80, t)
                    p = blend(p, tuple(min(c * shade, 255.0) for c in SCREEN_BG), 1.0)
                else:
                    p = blend(p, ACCENT, 1.0)

                # The chevron sits on top of the screen area.
                if chevron_contains(x, y, cx - 0.10 * n, cy, half_h, thickness, lean):
                    p = blend(p, ACCENT_BRIGHT, 1.0)
                if chevron_contains(x, y, cx + 0.10 * n, cy, half_h, thickness, lean):
                    p = blend(p, ACCENT, 1.0)

            px[row + ix] = p

    # Box-filter down to the target size. Doing the antialiasing here rather
    # than with per-shape coverage keeps every edge consistent.
    out = []
    inv = 1.0 / (SS * SS)
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0.0
            for sy in range(SS):
                base = (oy * SS + sy) * n + ox * SS
                for sx in range(SS):
                    sr, sg, sb, sa = px[base + sx]
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            a *= inv
            if a > 1e-6:
                # Un-premultiply back to straight alpha.
                scale = inv / a
                out.append((r * scale, g * scale, b * scale, a))
            else:
                out.append((0.0, 0.0, 0.0, 0.0))
    return out


def to_bmp_image(pixels, size):
    """Packs pixels into the BITMAPINFOHEADER + BGRA + AND-mask blob an ICO
    directory entry expects."""
    header = struct.pack(
        "<IiiHHIIiiII",
        40,            # biSize
        size,          # biWidth
        size * 2,      # biHeight: colour plus mask, per the ICO format
        1,             # biPlanes
        32,            # biBitCount
        0,             # biCompression = BI_RGB
        size * size * 4,
        0, 0, 0, 0,
    )

    # Bottom-up rows.
    body = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = pixels[y * size + x]
            body += bytes((
                int(round(min(max(b, 0.0), 255.0))),
                int(round(min(max(g, 0.0), 255.0))),
                int(round(min(max(r, 0.0), 255.0))),
                int(round(min(max(a * 255.0, 0.0), 255.0))),
            ))

    # AND mask: unused for 32-bit icons but must be present and row-padded to
    # four bytes.
    row_bytes = ((size + 31) // 32) * 4
    mask = bytes(row_bytes * size)
    return bytes(header) + bytes(body) + mask


def png_bytes(pixels, size):
    """Encodes 8-bit RGBA PNG. Standard library only, same as the rest of this
    file. Filter type 0 on every line: on this mark it compresses better than
    choosing a filter per line."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 for every scanline
        for x in range(size):
            r, g, b, a = pixels[y * size + x]
            raw += bytes((
                int(round(min(max(r, 0.0), 255.0))),
                int(round(min(max(g, 0.0), 255.0))),
                int(round(min(max(b, 0.0), 255.0))),
                int(round(min(max(a * 255.0, 0.0), 255.0))),
            ))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


def write_png(pixels, size, path):
    """The README needs a format GitHub will render, and .ico is not one."""
    with open(path, "wb") as f:
        f.write(png_bytes(pixels, size))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "res")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "qblank.ico")

    images = []
    largest = None
    for size in SIZES:
        print("  rendering {0}x{0}".format(size))
        pixels = render(size)
        if size == SIZES[-1]:
            largest = pixels
        image = png_bytes(pixels, size) if size >= PNG_FROM else to_bmp_image(pixels, size)
        images.append((size, image))

    # The same mark as a PNG, for the README. Transparent background, so it
    # reads on both the light and the dark GitHub theme.
    if largest is not None:
        docs_dir = os.path.join(root, "docs")
        os.makedirs(docs_dir, exist_ok=True)
        png_path = os.path.join(docs_dir, "icon.png")
        write_png(largest, SIZES[-1], png_path)
        print("  wrote {0}".format(png_path))

    offset = 6 + 16 * len(images)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    blobs = bytearray()
    for size, data in images:
        directory += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0,      # colour count
            0,      # reserved
            1,      # planes
            32,     # bit count
            len(data),
            offset,
        )
        blobs += data
        offset += len(data)

    with open(out_path, "wb") as f:
        f.write(bytes(directory) + bytes(blobs))
    print("geschrieben: {} ({} Bytes)".format(out_path, os.path.getsize(out_path)))


if __name__ == "__main__":
    main()
