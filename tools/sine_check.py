#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sine check: fit a 440 Hz sine per 10 ms block of a WAV recording; blocks below 30 dB SNR count as disturbed.

  tools/sine_check.py recording.wav [...]
"""
import sys, wave, numpy as np
for p in sys.argv[1:]:
    w=wave.open(p); fs=w.getframerate(); x=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).reshape(-1,2).astype(float)
    x=x[:, np.argmax(np.abs(x).sum(0))][fs:]/32768
    t=np.arange(len(x))/fs; snr=[]; bad=0; nb=len(x)//480
    for k in range(nb):
        sl=slice(k*480,(k+1)*480); A=np.c_[np.sin(2*np.pi*440*t[sl]),np.cos(2*np.pi*440*t[sl]),np.ones(480)]
        c=np.linalg.lstsq(A,x[sl],rcond=None)[0]; r=x[sl]-A@c
        v=10*np.log10((np.hypot(c[0],c[1])**2/2)/max(np.mean(r**2),1e-20)); snr.append(v); bad+= v<30
    print(f"{p.split('/')[-1]}: {len(x)/fs:.1f} s, 10ms blocks with SNR<30dB: {bad}/{nb} ({100*bad/nb:.1f}%), SNR median {np.median(snr):.1f} dB, min {min(snr):.1f} dB, peak {np.abs(x).max():.2f}")
