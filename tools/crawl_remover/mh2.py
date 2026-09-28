# Multi-hypothesis crawl estimator, second cut. Every hypothesis predicts the crawl of frame t from
# even-lag frames; each carries an estimate of its own error (noise it lets through plus how badly the
# frames disagree with its assumption, measured where the crawl cancels). Soft-min over the errors.
import numpy as np, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from proto import *
P = dict(sig2=0.4, tau=0.3, ksp=2.7, kb=1.0, kq=1.0, rb=2, raw=1, mc=1, s4=1, s3=1, dmax=8, kmc=0.7)
P.update({k: float(v) for k, v in (a.split('=') for a in os.environ.get('MHP', '').split() if a) if k in P})
def box(v, r):
    return sum(sh(v, k) for k in range(-r, r + 1)) / (2 * r + 1)
def boxv(v):  # three lines
    return (np.roll(v, 1, -2) + v + np.roll(v, -1, -2)) / 3
def B(v): return boxv(box(v, 3))
def box3(v): return (sh(v, -1) + v + sh(v, 1)) / 3
def demod(v, r):
    ks = np.arange(-r, r + 1); hw = 0.5 + 0.5 * np.cos(np.pi * ks / (r + 1)); hw /= hw.sum()
    x = np.arange(v.shape[-1])
    m = sum(h * sh(v, k) for h, k in zip(hw, ks))
    return sum(h * (sh(v, k) - m) * np.exp(-1j * W * (x + k)) for h, k in zip(hw, ks))
def remod(d):
    return 2 * np.real(d * np.exp(1j * W * np.arange(d.shape[-1])))
def at(v, pos):  # v sampled at float x positions pos (same shape), linear, clamped
    n = v.shape[-1]; p = np.clip(pos, 0, n - 1.001); i = np.floor(p).astype(int); f = p - i
    a = np.take_along_axis(v, i, -1); b = np.take_along_axis(v, i + 1, -1)
    return a + (b - a) * f
def motion(y0, y2, dmax):
    """Per pixel shift D (content of y0 at x sat at x - D in y2), sub-pixel, and its match cost."""
    a = box3(y0); cs = []
    for D in range(-dmax, dmax + 1):
        d = a - box3(sh(y2, -D)); cs.append(B(d * d))
    cs = np.stack(cs); i = np.clip(cs.argmin(0), 1, 2 * dmax - 1)
    c0 = np.take_along_axis(cs, i[None] - 1, 0)[0]; c1 = np.take_along_axis(cs, i[None], 0)[0]
    c2 = np.take_along_axis(cs, i[None] + 1, 0)[0]
    den = c0 - 2 * c1 + c2; off = np.where(den > 1e-6, 0.5 * (c0 - c2) / np.maximum(den, 1e-6), 0)
    off = np.clip(off, -0.5, 0.5)
    return i - dmax + off, c1, cs[dmax]
def mh2(L, rgb, dbg=False):
    out = L.copy(); rb = int(P['rb']); gw = []
    x = np.arange(L.shape[-1])
    for t in range(6, len(L)):
        y0, y2, y4, y6 = L[t], L[t - 2], L[t - 4], L[t - 6]
        sig2 = P['sig2']
        est, err = [], []
        def add(h, e_model, nv, raw=P['raw']):
            est.append(h if raw else remod(demod(h, rb)))
            err.append(np.maximum(e_model, 0) + nv * sig2)
        # static: the crawl flips every two frames
        h2 = (y0 - y2) / 2; r = y0 - y4
        add(h2, P['kb'] * B(box3(h2) ** 2) + P['kq'] * (B(r * r) / 4 - sig2 / 2), 0.5)
        if P['s3']:  # linear trend (fades): second difference
            h3 = (y0 - 2 * y2 + y4) / 4; r3 = y0 - y2 - y4 + y6
            add(h3, P['kb'] * B(box3(h3) ** 2) + P['kq'] * (B(r3 * r3) / 16 - sig2 / 4), 0.375)
        if P['s4']:  # static over six frames
            h4 = (y0 - y2 + y4 - y6) / 4; r4b = y2 - y6
            add(h4, P['kb'] * B(box3(h4) ** 2) + P['kq'] * (B(r * r + r4b * r4b) / 8 - sig2 / 2), 0.25)
        if P['mc']:  # translation D per two frames, solved at the carrier (the carrier stays on the raster,
            # the envelope moves with the picture and flips sign)
            D, cost, _ = motion(y0, y2, int(P['dmax']))
            d0 = demod(y0, rb); d2 = at(demod(y2, rb), x - D)
            rot = np.exp(-1j * W * D); g = 1 + rot
            e = (d0 - d2 * rot) * np.conj(g) / (np.abs(g) ** 2 + 0.05)
            # check two frames further back: y4 at x - 2D should hold the same content, rotated, and the crawl
            d4 = at(demod(y4, rb), x - 2 * D)
            pred4 = (d0 - e) * np.exp(2j * W * D) + e
            q = np.abs(d4 - pred4) ** 2
            nv = 1 / np.maximum(np.abs(g) ** 2, 0.2)
            add(remod(e), P['kb'] * cost / 4 + P['kq'] * (B(q) * 2 - 0.2 * sig2) + P['kmc'], nv, raw=1)
        # spatial fallback
        est.append(pattern(y0, 4)); err.append(np.full_like(y0, P['ksp']))
        E = np.stack(err); w = np.exp(-(E - E.min(0)) / P['tau']); w /= w.sum(0)
        out[t] = y0 - (w * np.stack(est)).sum(0)
        gw.append(w.mean((1, 2)))
    print('   weights', np.mean(gw, 0).round(2), file=sys.stderr)
    return out
