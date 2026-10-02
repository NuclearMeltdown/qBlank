# Writes qBlank's src/render/shaders_remover.h: the remover's three passes (dec, res, comb) for the model
# set mh5.models() gives under the current MHP, in qBlank's ConvertCB and binding layout.
#   dec   planes -> ring slot: float4(Y, Cb, Cr, lowpass Y), all in 8-bit units, expanded
#   res   ring t0..t11 newest first -> 8 MRT of per-fit residual energies
#   comb  ring t0..t11 + residuals t12..t19 -> cleaned RGB (expanded, as FetchRgbAtL gives it)
#
# usage: python gen_qb.py ../../src/render/shaders_remover.h
# The defaults below are the set qBlank ships; any of them can be overridden from the environment.
import sys, os, numpy as np
for k, v in dict(WH='0.03', AL='0.25', WC='1', NB='2 2', MHP='kmax=12 ks=5,6,7,8,10,12 mc=0').items():
    os.environ.setdefault(k, v)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mh5
OUT = sys.argv[1]
ms = mh5.models()
fits = []
for m in ms:
    key = (m['kind'], m['K'])
    if key not in fits: fits.append(key)
NF = len(fits); NRT = (NF + 3) // 4
assert NRT <= 8, NF
gfits = [(i, kind, K) for i, (kind, K) in enumerate(fits) if kind[0] == 'G']
GK = max([K for _, _, K in gfits], default=0)
def f(v): return '%.8g' % v
def outer(x): return 'float4x4(%s * %s.x, %s * %s.y, %s * %s.z, %s * %s.w)' % ((x, x) * 4)

common = r'''cbuffer ConvertCB : register(b0) {
  int gFormatKind; int gDeinterlaceMode; int gFieldIndex; int gBottomUp;
  int gCropLeft; int gCropTop; int gSrcWidth; int gSrcHeight;
  int gOutWidth; int gOutHeight; int gIsYuv; int gHavePrev;
  float gYOffset; float gYScale; float gCScale; float gPixelScale;
  int gCoSitedPhase; int gRotation; int gLineDouble; int gChromaSoft;
  float gTemporal; int gHistCount; float gDotNotch; float gCarrierPeriod;
  int gTransfer; int gGamut; float gMotionSlack; float gMotionSlope;
  int gMotionComp; int gAdaptChroma; float gBandwidth; float gCompareSplit;
  float4 gCoef;
  int gCompareAxis; int gCrawlCycle; int gMotionRows; int gChromaLinear;
  int gRemover; int gRemHist; int gRemNew; int gRemPad;
  float4 gRemP0;
  float4 gRemP1;
};
#define gHist gRemHist
#define gSig2 gRemP0.x
#define gTau gRemP0.y
#define gKsp gRemP0.z
#define gMarg gRemP0.w
#define gKfac gRemP1.x
#define gBlack gRemP1.y
#define gGmin gRemP1.z
#define gGlo gRemP1.w
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
int2 Clamp2(int2 p) { return clamp(p, int2(0, 0), int2(gSrcWidth - 1, gSrcHeight - 1)); }
float4 Basis(int k) {
  int m = k & 3;
  return float4(1.0, m == 0 ? 1.0 : (m == 2 ? -1.0 : 0.0), m == 1 ? 1.0 : (m == 3 ? -1.0 : 0.0),
                (k & 1) ? -1.0 : 1.0);
}
float4x4 Inv4(float4x4 a) {
  float4x4 b = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
  [unroll] for (int i = 0; i < 4; ++i) {
    float d = 1.0 / a[i][i];
    a[i] *= d; b[i] *= d;
    [unroll] for (int j = 0; j < 4; ++j) {
      if (j != i) {
        float f = a[j][i];
        a[j] -= f * a[i]; b[j] -= f * b[i];
      }
    }
  }
  return b;
}
float4x4 GainA(float4x4 A, bool flick) {
  return flick ? A : float4x4(A[0].xyz, 0, A[1].xyz, 0, A[2].xyz, 0, 0, 0, 0, 1);
}
float4 GainB(float4 b, bool flick) { return flick ? b : float4(b.xyz, 0); }
'''
ring = '\n'.join('Texture2D<float4> F%d : register(t%d);' % (k, k) for k in range(12)) + '\n' + \
    'float4 LF(int k, int2 p) {\n  int3 q = int3(p, 0);\n' + \
    ''.join('  if (k == %d) return F%d.Load(q);\n' % (k, k) for k in range(11)) + '  return F11.Load(q);\n}\n'

dec = r'''Texture2D<float4> tex0 : register(t0);
Texture2D<float4> tex1 : register(t1);
Texture2D<float4> tex2 : register(t2);
float3 YuvAt(int2 p) {
  p = Clamp2(p);
  if (gIsYuv == 0) {
    float3 rgb = (tex0.Load(int3(p, 0)).rgb - gYOffset) * gYScale;
    float y = dot(rgb, float3(0.299, 0.587, 0.114));
    return float3(y, (rgb.b - y) / 1.772, (rgb.r - y) / 1.402) * 255.0;
  }
  const bool odd = (p.x & 1) != 0;
  const int cx = p.x >> 1;
  float3 c;
  if (gFormatKind <= 2) {
    float4 t = tex0.Load(int3(cx, p.y, 0));
    float y = gFormatKind == 1 ? (odd ? t.w : t.y) : (odd ? t.z : t.x);
    float2 uv = gFormatKind == 0 ? t.yw : (gFormatKind == 1 ? t.xz : t.wy);
    c = float3(y, uv);
  } else if (gFormatKind == 3 || gFormatKind == 7) {
    c = float3(tex0.Load(int3(p, 0)).x, tex1.Load(int3(cx, p.y >> 1, 0)).xy) * gPixelScale;
  } else {
    c = float3(tex0.Load(int3(p, 0)).x, tex1.Load(int3(cx, p.y >> 1, 0)).x, tex2.Load(int3(cx, p.y >> 1, 0)).x);
  }
  return float3((c.x - gYOffset) * gYScale, (c.y - 0.5) * gCScale, (c.z - 0.5) * gCScale) * 255.0;
}
float4 main(VSOut i) : SV_Target {
  int2 p = int2(i.pos.xy);
  static const float hw[9] = {1, 2, 3, 3, 3, 3, 3, 2, 1};
  static const float vw[5] = {1, 2, 3, 2, 1};
  float s = 0;
  [unroll] for (int j = -2; j <= 2; ++j) {
    float r = 0;
    [unroll] for (int k = -4; k <= 4; ++k) r += hw[k + 4] * YuvAt(p + int2(k, j)).x;
    s += vw[j + 2] * r;
  }
  return float4(YuvAt(p), s / (21.0 * 9.0));
}
'''

res = [ring, 'struct Out { %s };' % ' '.join('float4 r%d : SV_Target%d;' % (i, i) for i in range(NRT)),
       'Out main(VSOut i) {',
       '  int2 p = int2(i.pos.xy);',
       '  float y[12], d[12], yb[12], g[12], ss[13], lp[12], cb[12], cr[12];',
       '  const float4 v0 = LF(0, p);',
       '  [unroll] for (int k = 0; k < 12; ++k) { float4 v = k > gHist ? v0 : LF(k, p); y[k] = v.x; cb[k] = v.y; cr[k] = v.z; lp[k] = v.w; }',
       '  float lp0 = lp[0] - gBlack;',
       '  ss[0] = 0;',
       '  [unroll] for (int k = 0; k < 12; ++k) {',
       '    d[k] = y[k] - y[0]; yb[k] = y[k] - gBlack; ss[k + 1] = ss[k] + d[k] * d[k];',
       '    g[k] = lp0 < gGlo ? -1.0 : clamp((lp[k] - gBlack) / max(lp0, 1e-3), 0.0, 4.0);',
       '  }',
       '  float R[%d];' % (NRT * 4),
       '  [unroll] for (int j = 0; j < %d; ++j) R[j] = 0;' % (NRT * 4)]
# WH set: the misfit split into the carrier band (H = y - box3 y) and the rest (L); motion misfits in L,
# a drifting or clipped crawl in H, so R = rL + WH rH forgives the crawl but not the motion.
WH = float(os.environ['WH']) if os.environ.get('WH') else None
# AL = alpha: the crawl part of the L misfit is forgiven only up to alpha times the crawl part of H (the carrier
# leaks little into box3), so a motion in the crawl's rhythm stays a misfit; unset = forgive all of it.
# WC: the colour misfit (box3 Cb, Cr, crawl part forgiven -- cross colour lives there) weighted in; unset = off.
AL = float(os.environ['AL']) if os.environ.get('AL') else None
WC = float(os.environ['WC']) if os.environ.get('WC') else None
# NB = "a c": the checks' own noise allowances, a * (crawl columns) * s2 off the alpha term and
# c * 2 * dof * s2 off the colour misfit (default 0 0 = none)
nA, nC = (float(v) for v in os.environ.get('NB', '0 0').split())
if WH is not None:
    res += ['  float dL[12], dH[12], sL[13], sH[13]; sL[0] = 0; sH[0] = 0;',
            '  const int2 pl = Clamp2(p - int2(1, 0)), pr = Clamp2(p + int2(1, 0));',
            '  const float4 n0 = LF(0, pl) + LF(0, pr);',
            '  [unroll] for (int k = 0; k < 12; ++k) {',
            '    const float4 n = k > gHist ? n0 : LF(k, pl) + LF(k, pr);',
            '    const float l = (n.x + y[k]) / 3.0;',
            '    dL[k] = l; dH[k] = y[k] - l;']
    if gfits and AL is not None: res.append('    aL[k] = l - gBlack; aH[k] = dH[k];')
    if WC is not None: res.append('    dB[k] = (n.y + cb[k]) / 3.0; dR[k] = (n.z + cr[k]) / 3.0;')
    if gfits and WC is not None: res.append('    aB[k] = dB[k]; aR[k] = dR[k];')
    res += ['  }',
            '  [unroll] for (int k = 11; k >= 0; --k) { dL[k] -= dL[0]; dH[k] -= dH[0];%s }' % (' dB[k] -= dB[0]; dR[k] -= dR[0];' if WC is not None else ''),
            '  [unroll] for (int k = 0; k < 12; ++k) { sL[k + 1] = sL[k] + dL[k] * dL[k]; sH[k + 1] = sH[k] + dH[k] * dH[k];%s }'
            % (' sC[k + 1] = sC[k] + dB[k] * dB[k] + dR[k] * dR[k];' if WC is not None else '')]
    decl = []
    if gfits and AL is not None: decl.append('  float aL[12], aH[12];')
    if WC is not None: decl.append('  float dB[12], dR[12], sC[13]; sC[0] = 0;')
    if gfits and WC is not None: decl.append('  float aB[12], aR[12];')
    at = res.index('  const int2 pl = Clamp2(p - int2(1, 0)), pr = Clamp2(p + int2(1, 0));')
    res[at:at] = decl
for i, (kind, K) in enumerate(fits):
    if kind[0] == 'G': continue
    X, ec, es = mh5.basis(kind, K)
    Q, _ = np.linalg.qr(X)
    if WH is not None:
        pc = 1 + ('T' in kind)   # the content columns come first in mh5.basis, so the first pc of Q span them
        body = ['float rl = sL[%d], rh = sH[%d]%s%s;' % (K, K, ', lc = 0, hc = 0' if AL is not None else '', ', rc = sC[%d]' % K if WC is not None else '')]
        for j in range(Q.shape[1]):
            q = Q[:, j]
            nz = [k for k in range(K) if abs(q[k]) > 1e-12]
            dot = lambda a: ' + '.join('%s * %s[%d]' % (f(q[k]), a, k) for k in nz)
            cr_ = AL is not None and j >= pc
            body.append('{ float t = %s; rl -= t * t;%s }' % (dot('dL'), ' lc += t * t;' if cr_ else ''))
            body.append('{ float t = %s; rh -= t * t;%s }' % (dot('dH'), ' hc += t * t;' if cr_ else ''))
            if WC is not None: body.append('{ float t = %s, u = %s; rc -= t * t + u * u; }' % (dot('dB'), dot('dR')))
        tail = 'max(rl, 0.0) + %s * max(rh, 0.0)' % f(WH)
        nb = lambda v: ' - %s * gSig2' % f(v) if v else ''
        if AL is not None: tail += ' + max(lc - %s * hc%s, 0.0)' % (f(AL), nb(nA * (Q.shape[1] - pc)))
        if WC is not None: tail += ' + %s * max(rc%s, 0.0)' % (f(WC), nb(nC * 2 * (K - Q.shape[1])))
        res.append('  if (gHist >= %d) { %s R[%d] = %s; }' % (K - 1, ' '.join(body), i, tail))
        continue
    body = ['float r = ss[%d];' % K]
    for q in (Q[:, j] for j in range(Q.shape[1])):
        body.append('{ float t = %s; r -= t * t; }' % ' + '.join('%s * d[%d]' % (f(q[k]), k) for k in range(K) if abs(q[k]) > 1e-12))
    res.append('  if (gHist >= %d) { %s R[%d] = r; }' % (K - 1, ' '.join(body), i))
if gfits and WH is not None and (AL is not None or WC is not None):
    # The gain fits get the same two checks: the crawl part of the L fit bounded by alpha times that of H,
    # and the colour fitted by the same gains (a colour change at about equal luma is no gain change).
    res.append('  float4x4 GA = float4x4(1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3); float4 Gb = 0; float Gyy = 0;%s%s'
               % (' float4 GL = 0, GH = 0;' if AL is not None else '', ' float4 GB = 0, GR = 0; float Gcc = 0;' if WC is not None else ''))
    for k in range(GK):
        acc = 'GA += %s; Gb += x * yb[%d]; Gyy += yb[%d] * yb[%d];' % (outer('x'), k, k, k)
        if AL is not None: acc += ' GL += x * aL[%d]; GH += x * aH[%d];' % (k, k)
        if WC is not None: acc += ' GB += x * aB[%d]; GR += x * aR[%d]; Gcc += aB[%d] * aB[%d] + aR[%d] * aR[%d];' % ((k,) * 6)
        res.append('  { float4 x = Basis(%d) * g[%d]; %s }' % (k, k, acc))
        for i, kind, K in gfits:
            if K != k + 1: continue
            fl = 'true' if 'f' in kind else 'false'
            s = ('  if (gHist >= %d) { float4x4 Ai = Inv4(GainA(GA, %s)); float4 bb = GainB(Gb, %s); float4 th = mul(Ai, bb);'
                 ' float r = max(Gyy - dot(bb, th) - 1e-3 * dot(th, th), 0.0);' % (K - 1, fl, fl))
            if AL is not None:
                s += (' { float4 l = GainB(GL, %s), h = GainB(GH, %s); float lc = dot(l, mul(Ai, l)) - l.x * l.x / GA[0][0],'
                      ' hc = dot(h, mul(Ai, h)) - h.x * h.x / GA[0][0]; r += max(lc - %s * hc%s, 0.0); }'
                      % (fl, fl, f(AL), ' - %s * gSig2' % f(nA * 3) if nA else ''))
            if WC is not None:
                s += (' { float4 b1 = GainB(GB, %s), b2 = GainB(GR, %s); r += %s * max(Gcc - dot(b1, mul(Ai, b1)) - dot(b2, mul(Ai, b2))%s, 0.0); }'
                      % (fl, fl, f(WC), ' - %s * gSig2' % f(nC * 2 * max(K - 4, 1)) if nC else ''))
            res.append(s + ' R[%d] = r; }' % i)
elif gfits:
    res.append('  float4x4 GA = float4x4(1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3); float4 Gb = 0; float Gyy = 0;')
    for k in range(GK):
        res.append('  { float4 x = Basis(%d) * g[%d]; GA += %s; Gb += x * yb[%d]; Gyy += yb[%d] * yb[%d]; }' % (k, k, outer('x'), k, k, k))
        for i, kind, K in gfits:
            if K != k + 1: continue
            fl = 'true' if 'f' in kind else 'false'
            res.append('  if (gHist >= %d) { float4 bb = GainB(Gb, %s); float4 th = mul(Inv4(GainA(GA, %s)), bb); R[%d] = Gyy - dot(bb, th) - 1e-3 * dot(th, th); }' % (K - 1, fl, fl, i))
res.append('  Out o;')
# Clamped: a half float target ends at 65504, and past a few hundred the excess decides nothing more.
for i in range(NRT): res.append('  o.r%d = clamp(float4(R[%d], R[%d], R[%d], R[%d]), 0.0, 60000.0);' % (i, 4 * i, 4 * i + 1, 4 * i + 2, 4 * i + 3))
res += ['  return o;', '}']

comb = [ring, '\n'.join('Texture2D<float4> RS%d : register(t%d);' % (i, 12 + i) for i in range(NRT)),
        'float4 RSL(int i, int3 q) {',
        ''.join('  if (i == %d) return RS%d.Load(q);\n' % (i, i) for i in range(NRT - 1)) + '  return RS%d.Load(q);' % (NRT - 1),
        '}',
        'float4 main(VSOut i) : SV_Target {',
        '  int2 p = int2(i.pos.xy);',
        '  float3 Y[12]; float g[12]; float lp[12];',
        '  const float4 v0 = LF(0, p);',
        '  [unroll] for (int k = 0; k < 12; ++k) { float4 v = k > gHist ? v0 : LF(k, p); Y[k] = v.xyz; lp[k] = v.w; }',
        '  float lp0 = lp[0] - gBlack;',
        '  [unroll] for (int k = 0; k < 12; ++k) g[k] = lp0 < gGlo ? -1.0 : clamp((lp[k] - gBlack) / max(lp0, 1e-3), 0.0, 4.0);',
        '  const float3 bk = float3(gBlack, 0.0, 0.0);',
        '  float4 P[%d];' % NRT,
        '  [unroll] for (int j = 0; j < %d; ++j) P[j] = 0;' % NRT,
        '  [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -3; dx <= 3; ++dx) {',
        '    int3 q = int3(Clamp2(p + int2(dx, dy)), 0);',
        '    [unroll] for (int j = 0; j < %d; ++j) P[j] += RSL(j, q);' % NRT,
        '  }',
        '  float R[%d];' % (NRT * 4),
        '  [unroll] for (int j = 0; j < %d; ++j) { R[4 * j] = P[j].x / 21.0; R[4 * j + 1] = P[j].y / 21.0; R[4 * j + 2] = P[j].z / 21.0; R[4 * j + 3] = P[j].w / 21.0; }' % NRT,
        '  float Emin = 1e30, sw = 0; float3 acc = 0;',
        '#define ACC(E_, O_) { float e_ = (E_); float3 o_ = (O_); if (e_ < Emin) { float sc = exp(-(Emin - e_) / gTau); sw = sw * sc + 1.0; acc = acc * sc + o_; Emin = e_; } else { float w_ = exp(-(e_ - Emin) / gTau); sw += w_; acc += w_ * o_; } }',
        '  float s2 = gSig2;']
def excess(i, kind, K):
    dof = [m for m in ms if (m['kind'], m['K']) == (kind, K)][0]['dof']
    return 'float ex = gKfac * max(R[%d] - %d * s2 * (1.0 + gMarg), 0.0);' % (i, dof)
for i, (kind, K) in enumerate(fits):
    if kind[0] == 'G': continue
    lines = [excess(i, kind, K)]
    for m in (m for m in ms if (m['kind'], m['K']) == (kind, K)):
        b = m['b']
        o = ' + '.join('%s * Y[%d]' % (f(b[k]), k) for k in range(K) if abs(b[k]) > 1e-12)
        lines.append('ACC(s2 * %s + %s * ex, %s);' % (f((b * b).sum()), f(m['kap']), o))
    comb.append('  if (gHist >= %d) {\n    %s\n  }' % (K - 1, '\n    '.join(lines)))
if gfits:
    comb.append('  float4x4 GA = float4x4(1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3, 0, 0, 0, 0, 1e-3); float4x3 S = 0; float gmn = 1e9, gmx = -1e9;')
    for k in range(GK):
        comb.append('  { float4 x = Basis(%d) * g[%d]; float3 yb = Y[%d] - bk; GA += %s; S += float4x3(x.x * yb, x.y * yb, x.z * yb, x.w * yb); gmn = min(gmn, g[%d]); gmx = max(gmx, g[%d]); }' % (k, k, k, outer('x'), k, k))
        for i, kind, K in gfits:
            if K != k + 1: continue
            fl = 'f' in kind
            lines = [excess(i, kind, K),
                     'float4x4 Am = GainA(GA, %s); float4x4 Ai = Inv4(Am);' % ('true' if fl else 'false'),
                     'float pen = (gmn < gGmin || gmx > 1.0 / gGmin) ? 1e4 : 0.0;',
                     'float4 mask = float4(1, 1, 1, %s);' % ('1' if fl else '0')]
            for m in (m for m in ms if (m['kind'], m['K']) == (kind, K)):
                if m['o'] == 'B':
                    lines.append('{ float4 r = Ai[0] * mask; ACC(s2 * Ai[0][0] + pen + %s * ex, bk + mul(r, S)); }' % f(m['kap']))
                else:
                    lines.append('{ float4 v = mul(float4(0, 1, 0, %s), Ai) * mask; float nv = 1.0 - 2.0 * g[0] * dot(v, float4(1, 1, 0, 1)) + dot(v, mul(Am, v)); ACC(s2 * nv + pen + %s * ex, Y[0] - mul(v, S)); }' % ('1' if fl else '0', f(m['kap'])))
            comb.append('  if (gHist >= %d) {\n    %s\n  }' % (K - 1, '\n    '.join(lines)))
# The spatial estimate doubles as the way out for a faint texture that moves: asphalt running towards the
# camera is a few levels deep, its residuals pass for noise, and the long averages drew it into streaks.
# Luma over three samples (the subcarrier out), the newest frame against one crawl cycle back (the crawl come
# round again), 7 x 3 spots of the same field, spread after the common part: where that exceeds 0.75 levels
# and the picture itself is faint (a moving edge fails the fits anyway, a fade keeps its average), the output
# leans to the spatial estimate. The convert pass asks the same of its gate (shaders.h, TemporalGate).
# And the same second question as there: what is left after fitting the difference as brightness and contrast
# of the picture itself, against room growing with the contrast. Sharp detail moving inside the spots (the gaps
# of a pulsing caption) passes the first test at any contrast; standing edges keep their history.
comb += [
    '  float3 sp;',
    '  {',
    '    const float kW = 6.283185307 / max(gCarrierPeriod, 1.5);',
    '    float3 m = 0, a = 0, b = 0; float hn = 0;',
    '    [unroll] for (int k = -4; k <= 4; ++k) { float h = 0.5 + 0.5 * cos(3.14159265 * k / 5.0); hn += h; m += h * LF(0, Clamp2(p + int2(k, 0))).xyz; }',
    '    m /= hn;',
    '    [unroll] for (int k = -4; k <= 4; ++k) { float h = 0.5 + 0.5 * cos(3.14159265 * k / 5.0); float3 v = LF(0, Clamp2(p + int2(k, 0))).xyz - m; float ph = kW * (p.x + k); a += h * v * cos(ph); b += h * v * sin(ph); }',
    '    float ph0 = kW * p.x;',
    '    float3 pat = 2.0 * (a * cos(ph0) + b * sin(ph0)) / hn;',
    '    sp = Y[0] - pat;',
    '    ACC(s2 + gKsp, sp);',
    '  }',
    '  float3 c = acc / sw;',
    '  if (gHist >= gCrawlCycle) {',
    '    float sd = 0.0, sdd = 0.0, sa = 0.0, saa = 0.0, sad = 0.0, ref = 0.0;',
    '    [unroll] for (int gr = -1; gr <= 1; ++gr) {',
    '      float l0[9], ln[9];',
    '      [unroll] for (int k = 0; k < 9; ++k) {',
    '        const int3 q = int3(Clamp2(p + int2(k - 4, 2 * gr)), 0);',
    '        l0[k] = F0.Load(q).x;',
    '        ln[k] = gCrawlCycle == 2 ? F2.Load(q).x : F4.Load(q).x;',
    '      }',
    '      if (gr == -1) ref = l0[0] + ln[0];',
    '      [unroll] for (int k2 = 0; k2 < 7; ++k2) {',
    '        const float p0 = l0[k2] + l0[k2 + 1] + l0[k2 + 2], pn = ln[k2] + ln[k2 + 1] + ln[k2 + 2];',
    '        const float d = p0 - pn, e = p0 + pn - 3.0 * ref;',
    '        sd += d; sdd += d * d; sa += e; saa += e * e; sad += e * d;',
    '      }',
    '    }',
    '    const float vdd = sdd - sd * sd / 21.0, vaa = saa - sa * sa / 21.0, vad = sad - sa * sd / 21.0;',
    '    const float moveRms = sqrt(max(vdd, 0.0) / 21.0) / 3.0;',
    '    const float contrast = sqrt(max(vaa, 0.0) / 21.0) / 6.0;',
    '    const float rest = sqrt(max(vdd - vad * vad / max(vaa, 1e-6), 0.0) / 21.0) / 3.0;',
    '    const float room = 0.75 + 0.03 * contrast;',
    '    c = lerp(c, sp, max(saturate((moveRms - 0.75) / 0.75) * (1.0 - saturate((contrast - 6.0) / 6.0)),',
    '                        saturate((rest - room) / room)));',
    '  }',
    '  c /= 255.0;',
    '  const float4 cf = gIsYuv != 0 ? gCoef : float4(1.402, 0.344136, 0.714136, 1.772);',
    '  return float4(c.x + cf.x * c.z, c.x - cf.y * c.y - cf.z * c.z, c.x + cf.w * c.y, 1.0);',
    '}']

def literal(text):
    # MSVC takes at most 16380 characters per piece of a string literal; adjacent pieces join.
    out, cur = [], ''
    for line in text.splitlines(True):
        if len(cur) + len(line) > 12000: out.append(cur); cur = ''
        cur += line
    out.append(cur)
    return '\n'.join('R"HLSL(%s)HLSL"' % c for c in out)
body = {'kRemDecPS': dec, 'kRemResPS': '\n'.join(res) + '\n', 'kRemCombPS': '\n'.join(comb) + '\n'}
h = ['#pragma once', '',
     '// The crawl remover ("Extended history"), generated by tools/crawl_remover/gen_qb.py from the',
     '// prototype (mh5.py), %d fits, %d candidates. Not edited by hand.' % (NF, len(ms) + 1), '',
     'namespace cap {', '']
for name, text in body.items():
    h.append('inline const char* %s =\n%s;\n' % (name, literal(common + text)))
h += ['}  // namespace cap', '']
open(OUT, 'w', newline='\n').write('\n'.join(h))
for name, text in body.items():
    print(name, len(common) + len(text), file=sys.stderr)
print('fits', NF, 'rts', NRT, 'candidates', len(ms) + 1, file=sys.stderr)
