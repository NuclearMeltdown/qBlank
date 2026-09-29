# Stress patterns for the crawl chain: clean 720x480 RGB clips (n, 480, 720, 3) uint8, as stdsim.py takes
# them. Each one aims at a place where averaging, the demodulator or the remover can go wrong.
# usage: patterns.py name n out.npy      names: see PATTERNS
import sys, numpy as np

W, H = 720, 480
SS = 2                                       # supersampling per axis, so fractional speeds give soft edges
P443, P358 = 13.5 / 4.43361875, 13.5 / 3.579545   # carrier periods in pixels

C = dict(red=(.85, .12, .12), blue=(.12, .16, .85), yel=(.9, .85, .12), grn=(.12, .75, .2), mag=(.8, .12, .75),
         cya=(.12, .75, .85), wht=(.9, .9, .9), blk=(.08, .08, .08), gry=(.5, .5, .5), pur=(.45, .15, .6))
PAIRS = [('red', 'blue'), ('blue', 'yel'), ('grn', 'mag'), ('red', 'cya'), ('wht', 'blue'), ('blk', 'yel'),
         ('red', 'grn'), ('mag', 'cya'), ('blue', 'grn'), ('yel', 'red'), ('wht', 'red'), ('cya', 'blue')]


def col(name):
    return np.array(C[name], np.float32)


def pick(mask, a, b):
    """a where mask, else b; a and b colour names or (h, w, 3) arrays"""
    a = col(a) if isinstance(a, str) else a; b = col(b) if isinstance(b, str) else b
    return np.where(mask[..., None], a, b).astype(np.float32)


def grid():
    x = (np.arange(W * SS, dtype=np.float32) + 0.5) / SS
    y = (np.arange(H * SS, dtype=np.float32) + 0.5) / SS
    return np.meshgrid(x, y)


def pinwheels(X, Y):
    """4x3 cells, twelve sectors each in two colours: edges at every angle, 15 degrees apart"""
    cx, cy = np.floor(X / 180), np.floor(Y / 160)
    ang = np.arctan2(Y - (cy + .5) * 160, X - (cx + .5) * 180) + np.pi
    odd = np.floor(ang / (np.pi / 6)).astype(int) % 2 == 1
    cell = (cy * 4 + cx).astype(int) % len(PAIRS)
    a = np.stack([col(p[0]) for p in PAIRS])[cell]; b = np.stack([col(p[1]) for p in PAIRS])[cell]
    return np.where(odd[..., None], a, b)


def glyphs(X, Y):
    """text-like blocks: 6x10 cells, gaps every third row"""
    gx, gy = np.floor(X / 6), np.floor(Y / 10)
    return ((gx * 7 + gy * 3) % 5 < 2) & (gy % 3 != 2) & (gx % 9 != 8)


def stripes(X, Y, deg, per, dx=0.0, dy=0.0):
    a = np.deg2rad(deg)
    return np.floor(((X - dx) * np.cos(a) + (Y - dy) * np.sin(a)) / per) % 2 == 1


def bands(Y, n):
    return np.floor(Y / (H / n)).astype(int)


def p01_edges(X, Y, t):
    return pinwheels(X, Y)


def p02_hmove(X, Y, t):
    # bands moving at a quarter, half and one carrier period of both carriers, and at 1 and 2 px per frame
    sp = np.array([P443 / 4, P443 / 2, P443, P358 / 4, P358 / 2, P358, 1.0, 2.0], np.float32)[bands(Y, 8)]
    i = (np.floor((X - sp * t) / 20) % 4).astype(int)
    return np.stack([col(c) for c in ('red', 'blue', 'yel', 'grn')])[i]


def p03_vscroll(X, Y, t):
    sp = np.array([0.5, 1.0, 2.0, 3.0], np.float32)[np.floor(X / 180).astype(int)]
    Ys = Y + sp * t
    i = (np.floor(Ys / 12) % 4).astype(int)
    bg = np.stack([col(c) for c in ('red', 'blue', 'grn', 'mag')])[i]
    return pick(glyphs(X, Ys), 'wht', bg)


def p04_diag(X, Y, t):
    left = pick(stripes(X, Y, 45, 6 / np.sqrt(2), t, t), 'red', 'blue')           # fine, moving (1, 1)
    rt = pick(stripes(X, Y, 30, 5), 'yel', 'blue')                                  # fine, still
    rb = pick(stripes(X, Y, 60, 16, 2 * t), 'grn', 'wht')                           # road lines, moving 2 px
    return np.where((X < 360)[..., None], left, np.where((Y < 240)[..., None], rt, rb))


def p05_movefade(X, Y, t):
    blocks = lambda v: (np.floor((X - v * t) / 40) % 2 == 0) & (np.floor(Y / 40) % 2 == 0)
    a1 = pick(blocks(2), 'blue', 'red')                  # blue on red, moving
    a2 = pick(blocks(0), 'blue', 'red')
    b2 = pick(blocks(0), 'yel', 'blue')
    b3 = pick(blocks(2), 'yel', 'blue')
    fade = np.float32(0.5 + 0.5 * np.cos(2 * np.pi * t / 40))    # whole band to black and back
    mix = np.float32(1 - abs((t % 40) / 20 - 1))                  # cross fade A -> B -> A
    top = a1 * fade
    mid = a2 * (1 - mix) + b2 * mix
    bot = a1 * (1 - mix) + b3 * mix
    return np.where((Y < 160)[..., None], top, np.where((Y < 320)[..., None], mid, bot))


def p06_overlay(X, Y, t):
    bg = pinwheels(X, Y)
    wob = pinwheels(X + 4 * np.sin(Y / 6 + t / 2), Y)             # the background, refracted
    x0 = (3 * t) % (W + 200) - 200
    inside = (X >= x0) & (X < x0 + 200) & (Y >= 100) & (Y < 260)
    glass = 0.5 * wob + 0.5 * col('cya')
    still = (X >= 200) & (X < 520) & (Y >= 320) & (Y < 440)     # a standing half-dark box with moving water
    dark = 0.5 * wob
    return np.where(inside[..., None], glass, np.where(still[..., None], dark, bg))


def p07_spring(X, Y, t):
    # objects that return to their start every 2, 4, 6 or 8 frames, and one bouncing up and down every 4
    b = bands(Y, 5)
    per = np.array([2, 4, 6, 8, 4], np.float32)[b]
    off = np.round(12 * (1 - np.cos(2 * np.pi * t / per)) / 2)
    Xs = np.where(b < 4, X - off, X); Ys = np.where(b == 4, Y - off, Y)
    obj = (np.floor(Xs / 60) % 2 == 0) & ((Ys % 96) > 30) & ((Ys % 96) < 66)
    return pick(obj, 'wht', pick((np.floor(Xs / 60) % 4 == 1), 'mag', 'pur'))


def p08_carrier(X, Y, t):
    # luma waves around both carriers, then dithers: 1 px checker, red/blue columns 1 and 2 px wide
    b = bands(Y, 8)
    per = np.array([2.8, P443, 3.3, P358, 4.0, 1, 1, 1], np.float32)[b]
    lum = 0.5 + 0.3 * np.cos(2 * np.pi * X / per)
    out = np.repeat(lum[..., None], 3, -1).astype(np.float32)
    xi, yi = np.floor(X), np.floor(Y)
    out = np.where((b == 5)[..., None], pick((xi + yi) % 2 == 0, 'wht', 'blk'), out)
    out = np.where((b == 6)[..., None], pick(xi % 2 == 0, 'red', 'blue'), out)
    out = np.where((b == 7)[..., None], pick(np.floor(xi / 2) % 2 == 0, 'red', 'blue'), out)
    return out


def p09_startstop(X, Y, t):
    # text on colour moving 0.5, 1, 2 and 3 px per frame: eight frames moving, eight standing
    moved = 8 * (t // 16) + min(t % 16, 8)
    sp = np.array([0.5, 1.0, 2.0, 3.0], np.float32)[bands(Y, 4)]
    Xs = X - sp * moved
    bg = np.stack([col(c) for c in ('red', 'blue', 'grn', 'mag')])[bands(Y, 4)]
    return pick(glyphs(Xs, Y), 'yel', bg)


def p10_band(X, Y, t):
    # luma chirp from a 2.6 to a 5 px period; the bottom half moves half a pixel per frame
    f0, f1 = 1 / 2.6, 1 / 5.0
    Xs = np.where(Y < 240, X, X - 0.5 * t)
    ph = f0 * Xs + (f1 - f0) * Xs * Xs / (2 * W)
    lum = 0.5 + 0.3 * np.cos(2 * np.pi * ph)
    return np.repeat(lum[..., None], 3, -1).astype(np.float32)


def p11_cut(X, Y, t):
    # hard cuts every 12 frames between the pinwheels and scrolled text
    return pinwheels(X, Y) if (t // 12) % 2 == 0 else p03_vscroll(X, Y, 0)


def p12_drop(X, Y, t):
    # half standing edges, half text moving 1 px per frame; the runner drops frames from the decode
    return np.where((Y < 240)[..., None], pinwheels(X, Y), pick(glyphs(X - t, Y), 'yel', 'blue'))


PATTERNS = {f.__name__: f for f in (p01_edges, p02_hmove, p03_vscroll, p04_diag, p05_movefade, p06_overlay,
                                    p07_spring, p08_carrier, p09_startstop, p10_band, p11_cut, p12_drop)}
DROPS = dict(p12_drop=(24, 36, 45))    # frame indices the capture loses


def make(name, n):
    X, Y = grid(); f = PATTERNS[name]
    out = np.empty((n, H, W, 3), np.uint8)
    for t in range(n):
        img = f(X, Y, t).reshape(H, SS, W, SS, 3).mean((1, 3))
        out[t] = np.clip(np.rint(img * 255), 0, 255)
    return out


if __name__ == '__main__':
    name, n, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    np.save(dst, make(name, n))
    print(name, n, flush=True)
