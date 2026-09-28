# Crawl remover, fifth cut. Same per pixel model family as mh4, but every model offers two outputs:
#   A  the frame minus the model's crawl (keeps the frame's own detail and noise)
#   B  the model's content at the newest frame (a temporal average: removes crawl, flicker and noise)
# Each output is scored against the clean picture: noise sigma^2 |b|^2 plus kappa * excess residual, with
# kappa = max over disturbances u of |b.u - u_target|^2 / |M u|^2. Soft-min over all outputs, the motion
# compensated estimate and the spatial fallback.
import numpy as np, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from proto import *
from mh2 import box, boxv, box3, demod, remod, at, motion
P = dict(sig2=0.32, tau=0.1, ksp=3.0, kmc=0.6, mc=1, kq=1.0, kb=1.0, dmax=8, rb=2, kmax=8, pool=1, excl='', only='',
         gain=1, kfac=1.0, black=1.0, gmin=0.02, glp=3, glo=4, marg=0.2, outs='AB', gflick=0, xdist=1, kinds='S,Sf,T,Tf,TX,TXf,G,Gf', ks='', lks='8,12', ldmin=2.0, lreg=1e-3)
P.update({k: (float(v) if k not in ('excl', 'only', 'outs', 'kinds', 'ks', 'lks') else v) for k, v in (a.split('=') for a in os.environ.get('MHP', '').split() if a)})
def B(v):
    return boxv(box(v, 3)) if P['pool'] else v
def content_dist(K):
    k = np.arange(K); us = []
    for k0 in range(1, K): us.append((k < k0).astype(float))
    for k0 in range(K): us.append((k == k0).astype(float))
    us.append((k / K) ** 2); us.append((k / K) ** 3)
    for k0 in range(1, K - 1): us.append(np.maximum(0, k0 - k).astype(float)); us.append(np.maximum(0, k - k0).astype(float))
    return us
def crawl_dist(K):
    k = np.arange(K).astype(float); us = [np.cos(np.pi * k)]
    if P['xdist']: us += [k * np.cos(np.pi * k / 2) / K, k * np.sin(np.pi * k / 2) / K]
    return us
def basis(kind, K):
    k = np.arange(K).astype(float); c, s, f = np.cos(np.pi * k / 2), np.sin(np.pi * k / 2), np.cos(np.pi * k)
    cols = [np.ones(K)]
    if 'T' in kind: cols.append(k)
    crawl = [len(cols), len(cols) + 1]; cols += [c, s]
    if 'f' in kind: crawl.append(len(cols)); cols.append(f)
    if 'X' in kind: crawl += [len(cols), len(cols) + 1]; cols += [k * c, k * s]
    X = np.stack(cols, 1); ec = np.zeros(X.shape[1]); ec[crawl] = X[0, crawl]
    es = np.zeros(X.shape[1]); es[0] = 1
    return X, ec, es
def kappa(b, M, K):
    kap = 0.0
    dl = [(u, u[0]) for u in content_dist(K)] + [(u, 0.0) for u in crawl_dist(K)]
    for u, u0 in dl:
        num = (b @ u - u0) ** 2; den = ((M @ u) ** 2).sum()
        if num > 1e-9 and den < 1e-9: return None
        if num > 1e-9: kap = max(kap, num / den)
    return kap
def models():
    out = []
    for kind in P['kinds'].split(','):
        if kind[0] == 'L':   # two static layers mixed by a per frame weight read off the lowpass
            for K in (int(v) for v in P['lks'].split(',')):
                p = 6 + 2 * ('f' in kind)
                if K - p < 1 or K > P['kmax']: continue
                X, ec, es = basis(kind.replace('L', 'TX'), K)
                pinv = np.linalg.pinv(X); M = np.eye(K) - X @ pinv; e0 = np.zeros(K); e0[0] = 1
                for o in P['outs']:
                    b = e0 - ec @ pinv if o == 'A' else es @ pinv
                    kap = kappa(b, M, K)
                    out.append(dict(name='%s%d%s' % (kind, K, o), kind=kind, K=K, o=o, dof=K - p, kap=kap if kap is not None else 3.0))
            continue
        for K in range(3, int(P['kmax']) + 1):
            if P['ks'] and str(K) not in P['ks'].split(','): continue
            X, ec, es = basis(kind.replace('G', 'S'), K); p = X.shape[1]
            if K - p < 1: continue
            pinv = np.linalg.pinv(X); M = np.eye(K) - X @ pinv
            e0 = np.zeros(K); e0[0] = 1
            for o in P['outs']:
                b = e0 - ec @ pinv if o == 'A' else es @ pinv
                kap = kappa(b, M, K)
                name = '%s%d%s' % (kind, K, o)
                if kap is None or name in P['excl'].split(',') or (P['only'] and kind not in P['only'].split(',')): continue
                out.append(dict(name=name, kind=kind, K=K, X=X, ec=ec, es=es, o=o, b=b, M=M, dof=K - p, kap=kap))
    return out
def gains(Ys):
    lp = np.stack([boxv(boxv(box(box3(v), int(P['glp'])))) for v in Ys]) - P['black']
    if P['gflick']:   # the frame rate flicker at colour edges survives the lowpass; take it out of the gain
        # and so does crawl whose envelope has low frequencies: take every four frame periodic part out
        K = len(Ys); k = np.arange(K); per = [np.cos(np.pi * k / 2), np.sin(np.pi * k / 2), np.cos(np.pi * k)]
        Xq = np.stack([np.ones(K), k, k * k] + per, 1); pq = np.linalg.pinv(Xq)
        for j, u in enumerate(per): lp = lp - u[:, None, None] * np.tensordot(pq[3 + j], lp, 1)
    # near black the ratio is noise: mark it unusable (the gain models are then out)
    g = np.clip(lp / np.maximum(lp[0], 1e-3), 0.0, 4.0); g[:, lp[0] < P['glo']] = -1
    return g
def solve_gain(Ys, g, X, ec, es, o):
    """LS of y_k = g_k * (X theta)_k per pixel (g_0 = 1). Returns the output and its noise gain."""
    Xg = g[..., None] * X[:, None, None, :]          # K,H,W,p
    A = np.einsum('khwp,khwq->hwpq', Xg, Xg) + np.eye(X.shape[1]) * 1e-3
    b = np.einsum('khwp,khw->hwp', Xg, Ys)
    Ai = np.linalg.inv(A); th = np.einsum('hwpq,hwq->hwp', Ai, b)
    fit = np.einsum('khwp,hwp->khw', Xg, th); R = ((Ys - fit) ** 2).sum(0)
    if o == 'B':
        return th @ es, R, np.einsum('p,hwpq,q->hw', es, Ai, es)
    # y0 - crawl0: weights on the frames are e0 - Xg @ Ai @ ec
    w = -np.einsum('khwp,hwpq,q->khw', Xg, Ai, ec); w[0] += 1
    return Ys[0] - th @ ec, R, (w * w).sum(0)
def solve_layer(Ys, lp, flick, o):
    """y_k = s + b_k D + crawl_k + b_k crawl'_k, b_k = (lp_k - lp_old) / (lp_0 - lp_old): two static layers
    (the old picture and whatever it is turning into) mixed by a weight the lowpass measures."""
    K = len(Ys); k = np.arange(K); c, sn, f = np.cos(np.pi * k / 2), np.sin(np.pi * k / 2), np.cos(np.pi * k)
    den = lp[0] - lp[K - 1]; ok = np.abs(den) >= P['ldmin']
    b = (lp - lp[K - 1]) / np.where(ok, den, 1.0)
    one = np.ones_like(b)
    cols = [one, b, c[:, None, None] * one, sn[:, None, None] * one, b * c[:, None, None], b * sn[:, None, None]]
    if flick: cols += [f[:, None, None] * one, b * f[:, None, None]]
    X = np.stack(cols, -1)                               # K,H,W,p
    p = X.shape[-1]
    A = np.einsum('khwp,khwq->hwpq', X, X) + np.eye(p) * P['lreg']
    Ai = np.linalg.inv(A); th = np.einsum('hwpq,khwq,khw->hwp', Ai, X, Ys)
    fit = np.einsum('khwp,hwp->khw', X, th); R = ((Ys - fit) ** 2).sum(0)
    if o == 'B':
        es = np.zeros(p); es[0] = 1; es[1] = 1          # content at k = 0 (b_0 = 1)
        return th @ es, R, np.einsum('p,hwpq,q->hw', es, Ai, es), ok
    ec = X[0].copy(); ec[..., 0] = 0; ec[..., 1] = 0     # crawl at k = 0
    w = -np.einsum('khwp,hwpq,hwq->khw', X, Ai, ec); w[0] += 1
    return Ys[0] - (th * ec).sum(-1), R, (w * w).sum(0), ok
def candidates(L, t, Ys, g, x, lp=None):
    s2 = P['sig2']; rb = int(P['rb']); est, err, names = [], [], []
    for m in MODELS:
        K, kind = m['K'], m['kind']
        if kind[0] == 'L':
            o, R, nv, ok = solve_layer(Ys[:K], lp[:K], 'f' in kind, m['o'])
            noise = s2 * nv + np.where(ok, 0, 1e4)
        elif kind[0] == 'G':
            o, R, nv = solve_gain(Ys[:K] - P['black'], g[:K], m['X'], m['ec'], m['es'], m['o'])
            if m['o'] == 'B': o = o + P['black']
            else: o = o + P['black']
            gmin = g[:K].min(0); gmax = g[:K].max(0)
            noise = s2 * nv + np.where((gmin < P['gmin']) | (gmax > 1 / P['gmin']), 1e4, 0)
        else:
            o = np.tensordot(m['b'], Ys[:K], 1)
            r = np.tensordot(m['M'], Ys[:K], 1); R = (r * r).sum(0)
            noise = s2 * (m['b'] ** 2).sum()
        R = B(R); excess = np.maximum(R - m['dof'] * s2 * (1 + P['marg']), 0)
        est.append(o); err.append(noise + P['kfac'] * m['kap'] * excess); names.append(m['name'])
    if P['mc']:
        y0, y2, y4 = L[t], L[t - 2], L[t - 4]
        D, cost, _ = motion(y0, y2, int(P['dmax']))
        d0 = demod(y0, rb); d2 = at(demod(y2, rb), x - D)
        rot = np.exp(-1j * W * D); gg = 1 + rot
        e = (d0 - d2 * rot) * np.conj(gg) / (np.abs(gg) ** 2 + 0.05)
        d4 = at(demod(y4, rb), x - 2 * D); pred4 = (d0 - e) * np.exp(2j * W * D) + e
        q = B(np.abs(d4 - pred4) ** 2)
        nv = s2 / np.maximum(np.abs(gg) ** 2, 0.2)
        est.append(y0 - remod(e)); err.append(s2 + nv + P['kb'] * cost / 4 + P['kq'] * q + P['kmc']); names.append('mc')
    est.append(L[t] - pattern(L[t], 4)); err.append(np.full_like(L[t], s2 + P['ksp'])); names.append('sp')
    return est, err, names
MODELS = None
def mh5(L, rgb, dbg=False):
    global MODELS
    MODELS = models()
    out = L.copy(); gw = []; x = np.arange(L.shape[-1])
    H = int(P['kmax'])
    for t in range(max(H - 1, int(os.environ.get('F0', 7))), len(L)):
        Ys = np.stack([L[t - k] for k in range(H)])
        g = gains(Ys) if P['gain'] else None
        lp = np.stack([boxv(boxv(box(box3(v), int(P['glp'])))) for v in Ys]) if 'L' in P['kinds'] else None
        est, err, names = candidates(L, t, Ys, g, x, lp)
        E = np.stack(err); w = np.exp(-(E - E.min(0)) / P['tau']); w /= w.sum(0)
        out[t] = (w * np.stack(est)).sum(0)
        gw.append(w.mean((1, 2)))
    gm = np.mean(gw, 0); top = np.argsort(-gm)[:6]
    print('   ', ' '.join('%s:%.2f' % (names[i], gm[i]) for i in top), file=sys.stderr)
    return out
if __name__ == '__main__':
    for m in models(): print(m['name'], 'dof', m['dof'], 'kappa %.2f' % m['kap'], 'noise %.3f' % (m['b'] ** 2).sum())
