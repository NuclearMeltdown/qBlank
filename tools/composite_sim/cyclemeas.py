# The app's crawl cycle measurement (video_renderer.cpp SampleCrawl/AnalyzeCrawl/JudgeCrawl) on a clip,
# plus the same differences at every lag 1..12 for the general version.
# usage: cyclemeas.py clip.npy period [first]
import sys, math, numpy as np

TEMPL = np.array([[1.0, 0.0, 1.0, 0.0, 0.0], [1.0, 1.0, 0.0, 1.0, 0.0], [0.5, 1.0, 0.5, 0.0, 0.0]])


def taps(period):
    reach = int(math.ceil(period)); k = np.arange(-reach, reach + 1)
    win = 0.5 + 0.5 * np.cos(np.pi * k / (reach + 1)); om = 2 * np.pi / period
    return reach, win, win * np.cos(om * k), win * np.sin(om * k)


def sites(h, w, reach):
    x0, x1, y0, y1 = reach + w // 16, w - reach - w // 16, h // 16, h - h // 16
    ys, xs = [], []
    for j in range(32):
        band = y0 + (y1 - y0) * (2 * j + 1) // 64
        for i in range(40):
            ys.append(min((band & ~1) | ((i + j) & 1), h - 1)); xs.append(x0 + (x1 - x0) * (2 * i + 1) // 80)
    return np.array(ys), np.array(xs)


def phasors(clip, period):
    """Per frame and site: (re, im, mean) as the app keeps them, on green."""
    T, h, w = clip.shape[:3]
    reach, win, tc, ts = taps(period); ys, xs = sites(h, w, reach)
    cols = xs[:, None] + np.arange(-reach, reach + 1)[None, :]
    Z = np.empty((T, len(ys), 3), np.float64)
    for t in range(T):
        v = np.asarray(clip[t, ys[:, None], cols, 1], np.float64)
        sw, sc, ss = v @ win, v @ tc, v @ ts
        m = sw / win.sum()
        Z[t, :, 0] = sc - m * tc.sum(); Z[t, :, 1] = ss - m * ts.sum(); Z[t, :, 2] = m
    return Z, win.sum()


def measure(clip, period, window=100, span_all=5):
    """span_all: frames over which a site's mean must hold for the lag 1..12 levels (5 as the app's cycle
    test; 13 holds it over every frame those lags reach)."""
    Z, sumw = phasors(clip, period)
    T = len(Z); out = []
    diff = np.zeros(5); dall = np.zeros(13); still = 0; stall = 0; frames = 0
    for t in range(12, T):
        z = Z[t]; m = np.stack([Z[t - k, :, 2] for k in range(5)])
        ok = (m.max(0) - m.min(0)) <= 5.0
        e = ((z[:, :2] - Z[t - 12, :, :2]) ** 2).sum(1)
        if not ok.any(): frames += 1; continue
        rec = e[ok]; med = np.partition(rec, len(rec) // 2)[len(rec) // 2]
        limit = max(med * 6.0, (sumw * 0.5) ** 2)
        use = ok & (e <= limit)
        for k in range(4): diff[k] += ((z[use, :2] - Z[t - 1 - k][use, :2]) ** 2).sum()
        diff[4] += e[use].sum()
        ua = use
        if span_all > 5:
            ma = np.stack([Z[t - k, :, 2] for k in range(span_all)])
            ua = use & ((ma.max(0) - ma.min(0)) <= 5.0)
        for k in range(1, 13): dall[k] += ((z[ua, :2] - Z[t - k][ua, :2]) ** 2).sum()
        still += int(use.sum()); stall += int(ua.sum()); frames += 1
        if frames == window:
            out.append(judge(diff, dall, still, sumw, stall))
            diff[:] = 0; dall[:] = 0; still = 0; stall = 0; frames = 0
    if frames >= window // 2: out.append(judge(diff, dall, still, sumw, stall))
    return out


def judge(diff, dall, still, sumw, stall=None):
    if still < 4000: return dict(verdict=0, why='too little still', still=still)
    d = diff / still; level = np.sqrt(d) * 2 / sumw
    lvl12 = np.sqrt(dall[1:] / max(stall if stall is not None else still, 1)) * 2 / sumw
    lo, hi = d[:4].min(), d[:4].max()
    r = dict(verdict=0, level=level, all=lvl12, still=still, fit=None)
    if not hi > lo * 1.3: r['why'] = 'no crawl above the noise'; return r
    err = [(((d - lo) / (hi - lo) - TEMPL[c]) ** 2 * np.array([1, 1, 1, 1, 4.0])).sum() for c in range(3)]
    c = int(np.argmin(err)); r['fit'] = err[c]
    if err[c] <= 0.1: r['verdict'] = c + 2
    else: r['why'] = 'no cycle fits'
    return r


def fmt(r):
    s = 'verdict %d' % r['verdict']
    if 'level' in r:
        s += '  levels 1..4,12: ' + ' '.join('%.2f' % v for v in r['level'])
        s += '  fit %s' % ('%.3f' % r['fit'] if r['fit'] is not None else '-')
        s += '\n    all lags 1..12: ' + ' '.join('%.2f' % v for v in r['all'])
    if 'why' in r: s += '  (%s)' % r['why']
    return s + '  still %d' % r['still']


if __name__ == '__main__':
    clip = np.load(sys.argv[1], mmap_mode='r'); per = float(sys.argv[2])
    a = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    for r in measure(clip[a:], per): print(fmt(r), flush=True)
