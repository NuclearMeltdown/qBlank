# Error of a chain output against a reference, luma in 8-bit steps, split by what the truth does around each
# pixel over the last 12 frames: still (changes < 1.5), slow (fades, no step >= 12), fast (the rest, where
# ghosts show). crawl = the error's amplitude at the carrier (Hann taps as the app's measurement).
# Against the same chain fed the crosstalk-free decode, the error is what the crawl leaves or breaks;
# against the truth it also holds what the chain does to motion anyway.
import numpy as np

YW = np.array([.299, .587, .114], np.float32)


def luma(c):
    out = np.empty(c.shape[:3], np.float32)
    for t in range(len(c)): out[t] = np.asarray(c[t], np.float32) @ YW
    return out


def carrier_amp(e, period):
    reach = int(np.ceil(period)); kk = np.arange(-reach, reach + 1)
    win = 0.5 + 0.5 * np.cos(np.pi * kk / (reach + 1)); om = 2 * np.pi / period
    tc, ts = win * np.cos(om * kk), win * np.sin(om * kk)
    zr = np.zeros_like(e); zi = np.zeros_like(e); m = np.zeros_like(e)
    for i, k in enumerate(kk):
        s = np.roll(e, -k, -1); zr += tc[i] * s; zi += ts[i] * s; m += win[i] * s
    m /= win.sum(); zr -= m * tc.sum(); zi -= m * ts.sum()
    return np.hypot(zr, zi) * 2 / win.sum()


def classes(YG, t0):
    """Masks (T - t0, H, W) for still / slow / fast from the truth luma."""
    T = len(YG); st, sl, fa = [], [], []
    for t in range(t0, T):
        w = YG[t - 12:t + 1]
        s = np.abs(w - YG[t]).max(0)
        step = np.abs(np.diff(w, axis=0)).max(0)
        still = s < 1.5
        slow = ~still & (step < 12)
        st.append(still); sl.append(slow); fa.append(~still & ~slow)
    return np.array(st), np.array(sl), np.array(fa)


def evaluate(Yo, Yr, period, masks, t0, margin=(16, 24)):
    e = Yo[t0:] - Yr[t0:]
    a = carrier_amp(e, period)
    my, mx = margin
    crop = (slice(None), slice(my, -my), slice(mx, -mx))
    e, a = e[crop], a[crop]
    r = {}
    for name, m in zip(('still', 'slow', 'fast'), masks):
        m = m[crop]; n = m.sum()
        if n == 0: r[name] = None; continue
        r[name] = dict(n=n, rms=float(np.sqrt((e[m] ** 2).mean())), crawl=float(np.sqrt((a[m] ** 2).mean())),
                       g8=float((np.abs(e[m]) > 8).mean() * 100), g16=float((np.abs(e[m]) > 16).mean() * 100))
    # pixels still through the whole window: the standing part of the carrier error and what moves around it
    allst = masks[0][crop].all(0)
    if allst.sum() > 100:
        em = e.mean(0)
        r['standing'] = float(np.sqrt((carrier_amp(em[None], period)[0][allst] ** 2).mean()))
        r['moving'] = float(np.sqrt((carrier_amp(e - em, period)[:, allst] ** 2).mean()))
    return r


def fmt_crawl(tag, r):
    """against the chain's own crosstalk-free output"""
    s = '%-6s crawl' % tag
    if 'moving' in r: s += ' | still moving %.2f standing %.2f' % (r['moving'], r['standing'])
    for name in ('still', 'slow', 'fast'):
        q = r.get(name)
        s += ' | %s %s' % (name, '-' if q is None else '%.2f' % q['crawl'])
    q = r.get('fast')
    if q: s += ' | fast rms %.2f >16 %.2f%%' % (q['rms'], q['g16'])
    return s


def fmt_truth(tag, r, r0):
    """against the truth: the chain on the decode, and on the crosstalk-free decode"""
    s = '%-6s truth' % tag
    for name in ('still', 'slow', 'fast'):
        q, q0 = r.get(name), r0.get(name)
        if q is None: continue
        s += ' | %s rms %.2f (%.2f)' % (name, q['rms'], q0['rms'])
        if name == 'fast': s += ' >16 %.2f%% (%.2f%%)' % (q['g16'], q0['g16'])
    return s
