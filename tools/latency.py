# SPDX-License-Identifier: Apache-2.0
"""Timing fits, alignment and glitch detection for measure_latency.py (numpy only)."""

from typing import NamedTuple

import numpy as np

SINC_TAPS = 32
PHAT_BETA = 0.5          # partial whitening: sharp peak for music, a pure tone stays visibly ambiguous
MAX_LAG_S = 0.5
MIN_LAG_S = -0.05
AMBIGUOUS_RATIO = 1.5


class Alignment(NamedTuple):
    lag_s: float          # how much later rx is than ref
    peak_ratio: float     # main correlation peak / best other local peak outside +-1 ms
    gain: float


def _lower_envelope_fit(x, t, quantile=0.01):
    x = np.asarray(x, dtype=np.float64)
    t = np.asarray(t, dtype=np.float64)
    x0, t0 = x[0], t[0]
    b, a0 = np.polyfit(x - x0, t - t0, 1)
    r = (t - t0) - (a0 + b * (x - x0))
    a_rel = a0 + np.quantile(r, quantile)
    return t0 + a_rel - b * x0, b


def capture_time_fit(ts, t, offset_s: float = 1.041e-3):
    """(a, b) with capture(ts) = a + b*ts: lower envelope of the packet arrivals minus the fixed path delay."""
    a, b = _lower_envelope_fit(ts, t)
    return a - offset_s, b


def arrival_fit(seq, t):
    """(a, b) with arrival(seq) = a + b*seq on the lower envelope of the SDU arrivals."""
    return _lower_envelope_fit(seq, t)


def _decimate(x, factor):
    if factor == 1:
        return np.asarray(x, dtype=np.float64)
    X = np.fft.rfft(x)
    X[int(len(x) * 0.45 / factor):] = 0    # cut-off at 0.45 x the output rate
    return np.fft.irfft(X, len(x))[::factor]


def _interp(x, pos):
    """Windowed-sinc interpolation of x at fractional positions pos (zero outside x)."""
    out = np.empty(len(pos))
    half = SINC_TAPS // 2
    k = np.arange(-half + 1, half + 1)
    for s in range(0, len(pos), 50000):
        p = pos[s:s + 50000]
        i0 = np.floor(p).astype(np.int64)
        frac = p - i0
        d = frac[:, None] - k[None, :]
        w = np.sinc(d) * (0.5 + 0.5 * np.cos(np.pi * d / (half + 1)))
        idx = i0[:, None] + k[None, :]
        ok = (idx >= 0) & (idx < len(x))
        v = np.where(ok, x[np.clip(idx, 0, len(x) - 1)], 0.0)
        out[s:s + 50000] = np.sum(v * w, axis=1)
    return out


def _to_rx_grid(ref, ref_t0, ref_fs, rx_t0, rx_fs, j):
    """ref sampled at the rx times rx_t0 + j/rx_fs (j may be negative)."""
    factor = max(1, int(round(ref_fs / rx_fs)))
    ref_d = _decimate(ref, factor)
    fs_d = ref_fs / factor
    return _interp(ref_d, (rx_t0 + j / rx_fs - ref_t0) * fs_d)


def aligned_ref(ref, ref_t0, ref_fs, rx_t0, rx_fs, n, lag_s):
    """ref delayed by lag_s on the rx grid: comparable sample by sample with rx[:n]."""
    return _to_rx_grid(ref, ref_t0 + lag_s, ref_fs, rx_t0, rx_fs, np.arange(n, dtype=np.float64))


def align(ref, ref_t0, ref_fs, rx, rx_t0, rx_fs) -> Alignment:
    rx = np.asarray(rx, dtype=np.float64)[:int(10 * rx_fs)]
    n = len(rx)
    lo, hi = int(np.floor(MIN_LAG_S * rx_fs)), int(np.ceil(MAX_LAG_S * rx_fs))
    j = np.arange(-hi, n - lo, dtype=np.float64)
    g = _to_rx_grid(ref, ref_t0, ref_fs, rx_t0, rx_fs, j)    # g[i] is ref at rx index i - hi
    nfft = 1 << int(np.ceil(np.log2(len(g) + n)))
    G = np.fft.rfft(g, nfft)
    R = np.fft.rfft(rx, nfft)
    C = R * np.conj(G)
    C /= np.maximum(np.abs(C), 1e-12) ** PHAT_BETA
    # c[s] = sum_i rx[i] g[i - s]; lag m (in rx samples) corresponds to s = m - hi (mod nfft)
    m = np.arange(lo, hi + 1)
    cm = np.fft.irfft(C, nfft)[(m - hi) % nfft]
    k = int(np.argmax(cm))
    frac = 0.0
    if 0 < k < len(cm) - 1:
        y0, y1, y2 = cm[k - 1], cm[k], cm[k + 1]
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    lag_s = (m[k] + frac) / rx_fs

    # Stronger whitening (beta 0.8) would turn the window edges of a pure tone into a seemingly unique peak
    peaks = np.flatnonzero((cm[1:-1] > cm[:-2]) & (cm[1:-1] >= cm[2:])) + 1
    far = peaks[np.abs(peaks - k) > 1e-3 * rx_fs]
    second = cm[far].max() if len(far) else 0.0
    ratio = float(cm[k] / second) if second > 0 else float("inf")

    a = aligned_ref(ref, ref_t0, ref_fs, rx_t0, rx_fs, n, lag_s)
    gain = float(np.dot(rx, a) / max(np.dot(a, a), 1e-20))
    return Alignment(float(lag_s), ratio, gain)


def glitch_blocks(ref_aligned, rx, fs, block_s: float = 0.01, rise_db: float = 20.0,
                  max_snr_db: float = 1.0) -> np.ndarray:
    """Indices of blocks with a 1 ms sub-block where the error is about as large as the signal
    (SNR < max_snr_db, rx vs. level-matched ref) AND more than rise_db above the median error.
    Sub-blocks keep a short click from drowning in a loud 10 ms block; the SNR bound keeps codec artefacts
    on transients out, the error bound keeps codec noise in quiet stretches out."""
    n = int(round(block_s * fs))
    sub = max(1, int(round(1e-3 * fs)))
    per = n // sub
    nb = min(len(ref_aligned), len(rx)) // n
    r = np.asarray(ref_aligned[:nb * n], dtype=np.float64)
    x = np.asarray(rx[:nb * n], dtype=np.float64)
    g = np.dot(x, r) / max(np.dot(r, r), 1e-20)
    rs = (g * r)[:nb * per * sub].reshape(nb, n)[:, :per * sub].reshape(nb, per, sub)
    es = (x - g * r)[:nb * per * sub].reshape(nb, n)[:, :per * sub].reshape(nb, per, sub)
    p_sig = np.mean(rs ** 2, axis=2)
    p_err = np.maximum(np.mean(es ** 2, axis=2), 1e-20)
    snr = 10 * np.log10(np.maximum(p_sig, 1e-20) / p_err)
    loud = p_sig > 10 ** (-50 / 10)
    if not loud.any():
        return np.array([], dtype=int)
    err_db = 10 * np.log10(p_err)
    med_err = np.median(err_db[loud])
    bad = loud & (snr < max_snr_db) & (err_db > med_err + rise_db)
    return np.flatnonzero(np.any(bad, axis=1))
