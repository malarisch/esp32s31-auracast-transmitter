# SPDX-License-Identifier: Apache-2.0
"""Tests for lc3.py (ctypes wrapper around the system liblc3)."""

import unittest

import numpy as np

import lc3


class Lc3Test(unittest.TestCase):
    def test_roundtrip_24k(self):
        fs, n = 24000, 240
        x = 0.5 * np.sin(2 * np.pi * 1000 * np.arange(50 * n) / fs).astype(np.float32)
        enc, dec = lc3.Encoder(fs), lc3.Decoder(fs)
        y = np.concatenate([dec.decode(enc.encode(x[k * n:(k + 1) * n], 60)) for k in range(50)])
        d = lc3.delay_samples(fs)
        self.assertEqual(d, 60)   # 2.5 ms at 24 kHz
        a, b = x[n:-n - d], y[n + d:-n]
        corr = np.dot(a, b) / np.sqrt(np.dot(a, a) * np.dot(b, b))
        self.assertGreater(corr, 0.99)

    def test_plc_on_none(self):
        dec = lc3.Decoder(24000)
        y = dec.decode(None)
        self.assertEqual(y.shape, (240,))
        self.assertEqual(y.dtype, np.float32)


if __name__ == "__main__":
    unittest.main()
