# Composite round trip per video standard, to test the crawl chain on standards the card here cannot
# deliver. Clean RGB frames are encoded the way a console's video encoder does it (the standard's
# subcarrier, line and frame timing, both fields or progressive fields), then decoded the way a capture
# card's decoder does it (adaptive line comb or notch for QAM; trap, bell filter and FM discriminator for
# SECAM), and handed back as the 8-bit RGB frames the card delivers.
# usage: stdsim.py variant truth.npy out.npy [first count]
#   writes out.npy (decoded, uint8) and out_T.npy (the truth in the variant's geometry, uint8)
#   env NOISE composite noise sigma in 8-bit steps (1.0), SEED, TAU BETA GAMMA (comb decision)
import sys, os, numpy as np

FH525 = 4.5e6 / 286.0
FH625 = 15625.0
F443 = 4433618.75
SR = 13.5e6
PAD = 96
VAR = {
    # 625/50 PAL B/G/I: U turns -1/4 per frame, V +1/4 (V switch, 625 lines odd)
    'pal':      dict(lines=625, fh=FH625, fsc=F443, mod='pal', comb=2),
    'pal_n':    dict(lines=625, fh=FH625, fsc=3582056.25, mod='pal', comb=2),
    'pal_m':    dict(lines=525, fh=FH525, cpl=227.25, mod='pal', comb=2),
    # PAL 60 as the GameCube makes it (281.75 cycles per line), and free running at 4.43361875 MHz
    'pal60':    dict(lines=525, fh=FH525, cpl=281.75, mod='pal', comb=2),
    # (the decoder still combs two lines apart, where the carrier is 0.562 instead of 0.5 cycles off)
    'pal60_fr': dict(lines=525, fh=FH525, fsc=F443, mod='pal', comb=2),
    # NTSC M; NTSC J differs only in the black level, the carrier timing is the same
    'ntsc':     dict(lines=525, fh=FH525, cpl=227.5, mod='ntsc', comb=1),
    'ntsc443':  dict(lines=525, fh=FH525, fsc=F443, mod='ntsc', comb=2),
    'secam':    dict(lines=625, fh=FH625, mod='secam'),
    # progressive consoles: 240p with the standard line (crawl stands still), NES-like third-cycle lines,
    # 288p PAL (half a cycle per field, stands still per frame)
    'p240':     dict(lines=525, fh=FH525, cpl=227.5, mod='ntsc', comb=1, field=262),
    'p240_3':   dict(lines=525, fh=FH525, cpl=227 + 1 / 3, mod='ntsc', comb=0, field=262),
    'p288':     dict(lines=625, fh=FH625, fsc=F443, mod='pal', comb=2, field=312),
}
for v in VAR.values():
    if 'fsc' not in v and 'cpl' in v: v['fsc'] = v['cpl'] * v['fh']
    if 'cpl' not in v and 'fsc' in v: v['cpl'] = v['fsc'] / v['fh']
    v['H'] = 576 if v['lines'] == 625 else 480
    v['a0'] = 23 if v['lines'] == 625 else 22
    v['x0'] = 132 if v['lines'] == 625 else 122


def resample_lines(a, n):
    """Linear resample along axis 1 to n lines, centres aligned."""
    m = a.shape[1]
    pos = (np.arange(n) + 0.5) * m / n - 0.5
    i0 = np.clip(np.floor(pos).astype(int), 0, m - 1); i1 = np.clip(i0 + 1, 0, m - 1)
    w = np.clip(pos - np.floor(pos), 0, 1).astype(np.float32)[None, :, None, None]
    return a[:, i0] * (1 - w) + a[:, i1] * w


def geometry(T, v):
    """Truth frames (n, 480, 720, 3) -> the frames the card would show for this variant, float 0..255."""
    T = np.asarray(T, np.float32)
    if 'field' in v:   # the console draws one field's worth of lines; the card weaves two fields
        src = T[:, 0::2]
        if v['H'] == 576: src = resample_lines(src, 288)
        return np.repeat(src, 2, axis=1)
    if v['H'] == 480: return T
    out = np.empty((T.shape[0], 576) + T.shape[2:], np.float32)
    out[:, 0::2] = resample_lines(T[:, 0::2], 288)
    out[:, 1::2] = resample_lines(T[:, 1::2], 288)
    return out


def lines_of(v, k, H):
    """Per frame line: absolute line number (V switch, SECAM patterns), start time in line periods,
    field count parity."""
    y = np.arange(H); p = y & 1; j = y >> 1
    if 'field' in v:
        q = 2 * k + p
        n = q * v['field'] + j + v['a0']
        return n, n.astype(np.float64), q & 1
    L = v['lines']   # lines start on whole line periods; the second field starts (L + 1) / 2 lines later
    n = k * L + p * ((L + 1) // 2) + j + v['a0']
    return n, n.astype(np.float64), p


class Filters:
    def __init__(self, W, prm):
        self.W = W
        f = np.fft.rfftfreq(W, 1 / SR)
        self.f = f; self.nbw = prm['nbw']
        rc = lambda f0, f1: np.where(f <= f0, 1.0, np.where(f >= f1, 0.0, 0.5 + 0.5 * np.cos(np.pi * (f - f0) / (f1 - f0))))
        g = lambda c, w: np.exp(-np.log(2) * ((f - c) / w) ** 2)
        self.HY = rc(5.0e6, 6.0e6)                     # encoder luma
        self.HCe = g(0.0, prm['cbw'])                  # encoder chroma lowpass
        self.HC = g(0.0, 1.3e6)                        # decoder chroma lowpass
        self.HL = g(0.0, 1.0e6)                        # low band luma for the comb decision
        self.HTRAP = g(4.33e6, 0.75e6)                 # SECAM luma trap (removed part)
        fp = np.maximum(f, 1.0)
        self.Hpre = (1 + 1j * fp / 85e3) / (1 + 1j * fp / 255e3)
        self.Hde = 1 / self.Hpre
        ff = np.fft.fftfreq(W, 1 / SR); fpos = np.maximum(ff, 1.0)
        F = fpos / 4.286e6 - 4.286e6 / fpos
        self.Hbell = np.where(ff > 0, 2.0 / (1 + 1j * 16 * F), 0)   # analytic output
    def bp(self, fsc):
        return np.exp(-np.log(2) * ((self.f - fsc) / self.nbw) ** 2)
    def run(self, a, Hf):
        return np.fft.irfft(np.fft.rfft(a, axis=1) * Hf, n=self.W, axis=1).astype(np.float32)


def shift(a, o):
    """Row y + o, mirrored at the ends (o even keeps the field)."""
    H = a.shape[0]; y = np.arange(H); i = y + o
    i = np.where(i < 0, y - o, np.where(i >= H, y - o, i))
    return a[i]


def box1(a, r):
    c = np.cumsum(np.pad(a, ((0, 0), (r + 1, r)), mode='edge'), axis=1, dtype=np.float64)
    return ((c[:, 2 * r + 1:] - c[:, :-2 * r - 1]) / (2 * r + 1)).astype(np.float32)


def rgb_yuv(rgb):
    R, G, B = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    Y = 0.299 * R + 0.587 * G + 0.114 * B
    return Y, B - Y, R - Y


def to_rgb(Y, by, ry):
    R = Y + ry; B = Y + by; G = (Y - 0.299 * R - 0.114 * B) / 0.587
    return np.clip(np.rint(np.stack([R, G, B], -1) * 255), 0, 255).astype(np.uint8)


def frame(v, k, rgb, flt, rng, prm, ref=False):
    """One frame: rgb (H, W, 3) float 0..1 -> decoded uint8 (H, W, 3). ref: also the same decoder's output
    without crosstalk (no chroma left in luma, no luma in chroma; same filters, comb weights and noise)."""
    H, W = rgb.shape[:2]; We = W + 2 * PAD
    ext = np.pad(rgb, ((0, 0), (PAD, PAD), (0, 0)), mode='edge')
    Y, by, ry = rgb_yuv(ext)
    n, tl, fpar = lines_of(v, k, H)
    x = np.arange(-PAD, W + PAD)
    noise = rng.standard_normal((H, We)).astype(np.float32) * (prm['noise'] / 255.0)
    Yl = flt.run(Y, flt.HY)
    if v['mod'] == 'secam':
        isb = (n % 2 == 0)[:, None]
        D = np.where(isb, 1.505 * by, -1.902 * ry)
        D = np.fft.irfft(np.fft.rfft(flt.run(D, flt.HCe), axis=1) * flt.Hpre, n=We, axis=1)
        frest = np.where(isb, 4.25e6, 4.40625e6); dev = np.where(isb, 230e3, 280e3)
        f = np.clip(frest + dev * D, 3.9e6, 4.756e6)
        inv = ((n % 3) == 2) ^ (fpar == 1)
        ph0 = (frest[:, 0] * (v['x0'] - PAD) / SR + 0.5 * inv)[:, None]
        cyc = ph0 + np.cumsum(f / SR, axis=1) - f[:, :1] / SR
        F = f / 4.286e6 - 4.286e6 / f
        A = 0.115 * np.abs(1 + 16j * F) / np.abs(1 + 1.26j * F)
        car = (A * np.cos(2 * np.pi * (cyc % 1.0))).astype(np.float32)
        comp = (Yl + car + noise).astype(np.float32)
        Yd = comp - flt.run(comp, flt.HTRAP)
        def fm(sig):
            an = np.fft.ifft(np.fft.fft(sig, axis=1) * flt.Hbell, axis=1)
            fi = np.angle(an[:, 1:] * np.conj(an[:, :-1])) * SR / (2 * np.pi)
            fi = np.concatenate([fi[:, :1], 0.5 * (fi[:, 1:] + fi[:, :-1]), fi[:, -1:]], 1)
            Dd = (fi - frest) / dev
            Dd = np.fft.irfft(np.fft.rfft(Dd, axis=1) * flt.Hde, n=We, axis=1)
            Dd = flt.run(Dd.astype(np.float32), flt.HC)
            Do = shift(Dd, -2)                   # the delay line: the other component, one field line up
            return np.where(isb, Dd, Do), np.where(isb, Do, Dd)
        Db, Dr = fm(comp)
        out = to_rgb(Yd, Db / 1.505, -Dr / 1.902)[:, PAD:PAD + W]
        if not ref: return out
        Db, Dr = fm((car + noise).astype(np.float32))
        Yr = Yd - (car - flt.run(car, flt.HTRAP))
        return out, to_rgb(Yr, Db / 1.505, -Dr / 1.902)[:, PAD:PAD + W]
    U = flt.run(0.492 * by, flt.HCe); V = flt.run(0.877 * ry, flt.HCe)
    s = np.where(n % 2 == 0, 1.0, -1.0)[:, None] if v['mod'] == 'pal' else np.ones((H, 1))
    cyc = (v['cpl'] * tl % 1.0)[:, None] + (v['fsc'] / SR) * (x + v['x0'])[None, :]
    th = 2 * np.pi * (cyc % 1.0)
    sn, cs = np.sin(th).astype(np.float32), np.cos(th).astype(np.float32)
    Cc = (U * sn + s * V * cs).astype(np.float32)
    comp = (Yl + Cc + noise).astype(np.float32)
    bpf = flt.bp(v['fsc'])
    b = flt.run(comp, bpf)
    def demod(c):
        return flt.run(2 * c * sn, flt.HC), flt.run(2 * c * cs, flt.HC) * s
    comb = v['comb'] if prm['comb'] < 0 else int(prm['comb'])
    cf = lambda x: x
    if comb:
        d = 2 * comb
        ub, vb = demod(b); ly = flt.run(comp, flt.HL)
        def mis(o):
            return box1(np.abs(ly - shift(ly, o)) + np.abs(ub - shift(ub, o)) + np.abs(vb - shift(vb, o)), 2)
        mu, md = mis(-d), mis(d)
        cost = np.stack([mu + md, 2 * mu + prm['beta'], 2 * md + prm['beta'], np.full_like(mu, prm['gamma'])])
        w = np.exp(-(cost - cost.min(0)) / prm['tau']); w /= w.sum(0)
        def cf(x):
            xu, xd = shift(x, -d), shift(x, d)
            return w[0] * (2 * x - xu - xd) / 4 + w[1] * (x - xu) / 2 + w[2] * (x - xd) / 2 + w[3] * x
    c = cf(b)
    def finish(Yd, c):
        u, vv = demod(c)
        if v['mod'] == 'pal':
            u = 0.5 * (u + shift(u, -2)); vv = 0.5 * (vv + shift(vv, -2))
        return to_rgb(Yd, u / 0.492, vv / 0.877)[:, PAD:PAD + W]
    Yd = comp - c
    out = finish(Yd, c)
    if not ref: return out
    cC = cf(flt.run(Cc, bpf))                    # (the comb is linear once its weights are set)
    return out, finish(Yd - Cc + cC, c - cf(flt.run(Yl, bpf)))


# decoder and encoder knobs: noise sigma (8-bit steps), comb decision (tau, beta, gamma), encoder chroma
# bandwidth cbw, decoder chroma bandpass nbw (both Gaussian half widths in Hz), comb (-1 = the variant's)
# (nbw 0.65 MHz and noise 1.7 put simulated PAL 60 on the title screen where a real capture is)
PRM = dict(noise=1.7, tau=0.01, beta=0.015, gamma=0.06, cbw=1.3e6, nbw=0.65e6, comb=-1)


def env_prm():
    return {k: float(os.environ[k.upper()]) for k in PRM if k.upper() in os.environ}


def simulate(name, T, first=0, prm=None, ref=False):
    """-> decoded, truth in the variant's geometry (and with ref the crosstalk-free decode), all uint8"""
    v = VAR[name]
    prm = dict(dict(PRM), **(prm or {}))
    G = geometry(T, v)
    flt = Filters(G.shape[2] + 2 * PAD, prm)
    rng = np.random.default_rng(int(os.environ.get('SEED', 1)))
    out = np.empty(G.shape, np.uint8)
    rf = np.empty(G.shape, np.uint8) if ref else None
    for k in range(len(G)):
        r = frame(v, first + k, G[k] / 255.0, flt, rng, prm, ref)
        if ref: out[k], rf[k] = r
        else: out[k] = r
    G = np.clip(np.rint(G), 0, 255).astype(np.uint8)
    return (out, G, rf) if ref else (out, G)


if __name__ == '__main__':
    name, src, dst = sys.argv[1:4]
    a = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    T = np.load(src, mmap_mode='r')
    b = a + int(sys.argv[5]) if len(sys.argv) > 5 else len(T)
    out, G = simulate(name, T[a:b], 0, env_prm())
    np.save(dst, out); np.save(dst.replace('.npy', '_T.npy'), G)
    print(name, out.shape, flush=True)
