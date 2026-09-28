import numpy as np
W = 2 * np.pi / 3.0449
Y = np.array([.299, .587, .114], np.float32)
def sh(v, k):  # v(x+k), edge clamped
    if k == 0: return v
    n = v.shape[-1]; idx = np.clip(np.arange(n) + k, 0, n - 1)
    return v[..., idx]
def pattern(v, r, rot=None, weight=None):
    """DotDemodDelta's estimate of the carrier in v (last axis = x). rot: extra phase (per pixel) the
    carrier in v is known to be shifted by relative to the target, weight: gain to apply."""
    ks = np.arange(-r, r + 1); hann = 0.5 + 0.5 * np.cos(np.pi * ks / (r + 1)); norm = hann.sum()
    x = np.arange(v.shape[-1])
    mean = sum(h * sh(v, k) for h, k in zip(hann, ks)) / norm
    acc = sum(h * (sh(v, k) - mean) * np.cos(W * (x + k)) for h, k in zip(hann, ks))
    accq = sum(h * (sh(v, k) - mean) * np.sin(W * (x + k)) for h, k in zip(hann, ks))
    ph = W * x + (0 if rot is None else rot)
    p = 2 * (acc * np.cos(ph) + accq * np.sin(ph)) / norm
    return p if weight is None else p * weight
