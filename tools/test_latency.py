# SPDX-License-Identifier: Apache-2.0
"""Tests for latency.py and measure_latency.evaluate() on synthetic captures."""

import struct
import types
import unittest

import numpy as np

import capture
import latency
import lc3
import measure_latency
from test_capture import iso_rx, rtp, udp_frame

FS = 48000


def pink(n, seed=1):
    rng = np.random.default_rng(seed)
    w = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(w))
    w[1:] /= np.sqrt(f[1:])
    w[0] = 0
    x = np.fft.irfft(w, n)
    return 0.2 * x / np.std(x)


def brown(n, seed=1):
    """1/f^2 noise: falls off towards high frequencies like music, so LC3 reaches music-like SNRs."""
    rng = np.random.default_rng(seed)
    w = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(w))
    w[1:] /= np.maximum(f[1:], 20)
    w[0] = 0
    x = np.fft.irfft(w, n)
    return 0.2 * x / np.std(x)


def lowpass_decimate2(x):
    """Reference decimation for test data: brick-wall FFT low-pass at 11 kHz, then every 2nd sample."""
    X = np.fft.rfft(x)
    X[int(len(x) * 11000 / FS):] = 0
    return np.fft.irfft(X, len(x))[::2]


class FitTest(unittest.TestCase):
    def test_capture_fit_lower_envelope(self):
        rng = np.random.default_rng(3)
        k = np.arange(5000)
        ts = 48 * k
        t = 10.0 + k * 1e-3 + 1.041e-3 + rng.exponential(80e-6, len(k))
        a, b = latency.capture_time_fit(ts, t)
        self.assertAlmostEqual(a, 10.0, delta=20e-6)
        self.assertAlmostEqual(b, 1.0 / FS, delta=1e-9)


class AlignTest(unittest.TestCase):
    def test_align_known_lag(self):
        ref = pink(6 * FS)
        rng = np.random.default_rng(5)
        lag = 0.03725
        start = int(round((0.5 - lag) * FS))   # rx sample j is ref at time 0.5 + j/24000 - lag
        rx = lowpass_decimate2(ref)[start // 2:start // 2 + 4 * 24000]
        rx = rx + rng.standard_normal(len(rx)) * 0.002
        al = latency.align(ref, 0.0, FS, rx, 0.5, 24000)
        self.assertLess(abs(al.lag_s - lag), 50e-6)
        self.assertGreater(al.peak_ratio, 3)
        self.assertAlmostEqual(al.gain, 1.0, delta=0.05)

    def test_periodic_is_ambiguous(self):
        n = np.arange(6 * FS)
        ref = 0.3 * np.sin(2 * np.pi * 440 * n / FS)
        rng = np.random.default_rng(6)
        rx = ref[1788:1788 + 4 * FS] + rng.standard_normal(4 * FS) * 1e-4
        al = latency.align(ref, 0.0, FS, rx, 0.0, FS)
        self.assertLess(al.peak_ratio, 1.5)


class CandidateTest(unittest.TestCase):
    """latency.glitch_blocks(): candidates by SNR and error level."""

    def test_glitch_blocks(self):
        fs = 24000
        ref = pink(3 * fs, seed=7)
        rng = np.random.default_rng(8)
        rx = ref + rng.standard_normal(len(ref)) * 0.01
        self.assertEqual(list(latency.glitch_blocks(ref, rx, fs)), [])
        for b in (40, 120, 250):
            rx[b * 240 + 50:b * 240 + 60] = 0.9
        self.assertEqual(list(latency.glitch_blocks(ref, rx, fs)), [40, 120, 250])


class CandidateQuietTest(unittest.TestCase):
    def test_quiet_subblock_not_a_glitch(self):
        """Codec noise in a short quiet stretch of a loud block is not a click."""
        fs = 24000
        ref = pink(3 * fs, seed=14)
        for b in range(0, 300, 7):
            ref[b * 240 + 100:b * 240 + 148] *= 0.03      # 2 ms at about -44 dBFS inside the block
        rx = ref + np.random.default_rng(15).standard_normal(len(ref)) * 10 ** (-52 / 20)
        self.assertEqual(list(latency.glitch_blocks(ref, rx, fs)), [])


    def test_codec_transient_not_a_glitch(self):
        """A codec artefact well below the signal (about 10 dB SNR in 1 ms) is not a click."""
        fs = 24000
        ref = pink(3 * fs, seed=16)
        rx = ref + np.random.default_rng(17).standard_normal(len(ref)) * 10 ** (-52 / 20)
        for b in (30, 90, 200):
            seg = slice(b * 240 + 50, b * 240 + 74)
            rx[seg] += 0.3 * ref[seg] * np.sign(np.sin(np.arange(24)))
        self.assertEqual(list(latency.glitch_blocks(ref, rx, fs)), [])


    def test_dropout_is_a_glitch(self):
        fs = 24000
        ref = pink(3 * fs, seed=18)
        rx = ref + np.random.default_rng(19).standard_normal(len(ref)) * 10 ** (-52 / 20)
        rx[150 * 240 + 60:150 * 240 + 108] = 0.0    # 2 ms dropout
        self.assertEqual(list(latency.glitch_blocks(ref, rx, fs)), [150])


class GlitchTest(unittest.TestCase):
    """measure_latency.glitches(): blocks where the received audio is much worse than liblc3 on the reference."""

    def esp_like(self, ref, fs, nbytes):
        """What an ideal transmitter would deliver: ref through liblc3, re-aligned by the codec delay."""
        n, d = fs // 100, lc3.delay_samples(fs)
        enc, dec = lc3.Encoder(fs), lc3.Decoder(fs)
        src = np.concatenate([ref[d:], np.zeros(d)]).astype(np.float32)
        return np.concatenate([dec.decode(enc.encode(src[k * n:(k + 1) * n], nbytes)) for k in range(len(src) // n)])

    def test_decoder_warmup_not_a_glitch(self):
        fs = 48000
        ref = pink(2 * fs, seed=9)
        rx = self.esp_like(ref, fs, 120)
        rx[:120] = 0.0    # a cold LC3 decoder outputs ~2.5 ms of silence first
        self.assertEqual(measure_latency.glitches(ref, rx, fs, 120), [])

    def test_click_and_dropout(self):
        fs = 24000
        ref = brown(3 * fs, seed=21)
        rx = self.esp_like(ref, fs, 60)
        rx[100 * 240 + 50:100 * 240 + 56] = 0.9      # click
        rx[200 * 240 + 60:200 * 240 + 108] = 0.0     # 2 ms dropout
        self.assertEqual(measure_latency.glitches(ref, rx, fs, 60), [100, 200])


def synth_capture(delay_s, drop_sdus=(), drop_rtp=(), seconds=4, hq_bis=None, burst_at=None, click_at=None,
                  lost_sdus=(), restart_at=None, ts_jump_at=None, silent_tx=False,
                  content_step_at=None, content_step_s=0.0):
    """RTP stream plus one mono 24_2_1 BIS; SDU n arrives delay_s after the I2S time of its first sample.
    hq_bis=(c0, c1): instead two 48_4_1 BIS carrying reference channels c0 and c1 (L and R differ).
    burst_at=k: block k of the programme is quiet, then a loud noise attack: every LC3 encoder spreads
                pre-echo over the quiet half.
    click_at=k: a click in block k of the transmitted audio only (an error on the ESP side).
    lost_sdus: SDUs the PC controller reports as lost (status 2, no data).
    restart_at=n: the BIG restarts at SDU n (sequence numbers start again at 0).
    content_step_at=n, content_step_s: from SDU n on, the transmitted audio is content_step_s older
                                       (the bridge's delay steps up, e.g. a FIFO re-prime).
    ts_jump_at=k: the AES67 sender restarts at packet k with a new random timestamp base.
    silent_tx: the transmitted audio is digital silence."""
    ref = pink((seconds + 1) * FS, seed=11)
    if burst_at is not None:
        k = burst_at * 480
        ref[k:k + 240] *= 0.01
        ref[k + 240:k + 480] += np.random.default_rng(20).standard_normal(240) * 0.5
    stereo = np.stack([ref, ref if hq_bis is None else pink((seconds + 1) * FS, seed=13)], axis=1)
    pk = []
    t_cap0 = 100.0            # PTP capture time of ts=TS0, expressed on the PC clock
    ts0 = 0x7FFF0000
    rng = np.random.default_rng(12)
    for k in range(seconds * 1000):
        if k in drop_rtp:
            continue
        blk = np.clip(np.round(stereo[48 * k:48 * k + 48] * 8388607), -8388608, 8388607).astype(np.int32)
        payload = b"".join(int(v & 0xFFFFFF).to_bytes(3, "big") for v in blk.reshape(-1))
        t = t_cap0 + 48 * k / FS + 1.041e-3 + rng.exponential(60e-6)
        ts = ts0 + 48 * k if ts_jump_at is None or k < ts_jump_at else 0x12345678 + 48 * k
        pk.append(capture.Packet(0, 1, t, udp_frame("239.69.0.1", 5004, rtp(k & 0xFFFF, ts & 0xFFFFFFFF, payload))))
    if hq_bis is None:
        tx = lowpass_decimate2(ref)
        if click_at is not None:
            tx[click_at * 240 + 100:click_at * 240 + 106] = 0.9
        if silent_tx:
            tx[:] = 0.0
        streams = [(0x0010, lc3.Encoder(24000), tx, 240, 60)]
    else:
        streams = [(0x0010 + i, lc3.Encoder(FS), stereo[:, c], 480, 120) for i, c in enumerate(hq_bis)]
    i2s0 = t_cap0 + 2e-3      # I2S time of ref[0]
    for n in range(seconds * 100 - 10):
        for handle, enc, x, nf, nb in streams:
            o = nf * n
            if content_step_at is not None and n >= content_step_at:
                o -= int(round(content_step_s * nf * 100))
            sdu = enc.encode(x[o:o + nf], nb)
            if n in drop_sdus:
                continue
            t = i2s0 + n * 0.01 + delay_s + rng.exponential(30e-6)
            seq = n if restart_at is None or n < restart_at else n - restart_at
            if n in lost_sdus:
                pk.append(capture.Packet(1, 254, t, iso_rx(handle, seq, b"", status=2)))
            else:
                pk.append(capture.Packet(1, 254, t, iso_rx(handle, seq, sdu)))
    pk.sort(key=lambda p: p.t)
    return pk


ARGS = types.SimpleNamespace(group="239.69.0.1", port=5004, channels=2, pd_us=20000, fifo_frames=960,
                             transport_latency_us=None)


class EvaluateTest(unittest.TestCase):
    def test_known_latency(self):
        # The receiver joins a running stream: its decoder starts cold on SDU 50
        r = measure_latency.evaluate(synth_capture(0.0300, drop_sdus=range(50)), ARGS)
        # 30 ms SDU delay + 2.5 ms LC3 algorithmic delay
        self.assertAlmostEqual(r.channels[0].i2s_to_arrival_s, 0.0325, delta=100e-6)
        self.assertFalse(r.channels[0].ambiguous)
        self.assertEqual(r.channels[0].glitch_blocks, [])   # decoder start-up is not a glitch

    def test_gap_keeps_latency(self):
        clean = measure_latency.evaluate(synth_capture(0.0300), ARGS)
        gaps = measure_latency.evaluate(synth_capture(0.0300, drop_sdus=(50, 51, 120, 200, 333),
                                                      drop_rtp=(700, 1500, 2501)), ARGS)
        self.assertAlmostEqual(gaps.channels[0].i2s_to_arrival_s, clean.channels[0].i2s_to_arrival_s, delta=50e-6)
        self.assertEqual(gaps.channels[0].missing_sdus, 5)
        self.assertEqual(gaps.missing_rtp, 3)

    def test_channel_swap_detected(self):
        r = measure_latency.evaluate(synth_capture(0.0300, hq_bis=(1, 0), drop_sdus=range(50)), ARGS)
        self.assertEqual([c.source for c in r.channels], ["R", "L"])
        for c in r.channels:
            self.assertAlmostEqual(c.i2s_to_arrival_s, 0.0325, delta=100e-6)
            self.assertFalse(c.ambiguous)
            self.assertEqual(c.glitch_blocks, [])   # cold decoder start on a running stream is not a glitch

    def test_codec_limited_burst_not_a_glitch(self):
        r = measure_latency.evaluate(synth_capture(0.0300, burst_at=150), ARGS)
        self.assertEqual(r.channels[0].glitch_blocks, [])

    def test_click_on_esp_side_is_a_glitch(self):
        r = measure_latency.evaluate(synth_capture(0.0300, click_at=200), ARGS)
        self.assertEqual(len(r.channels[0].glitch_blocks), 1)
        self.assertLessEqual(abs(r.channels[0].glitch_blocks[0] - 200), 1)   # block index on the rx timeline

    def test_silence_is_ambiguous(self):
        al = latency.align(np.zeros(6 * FS), 0.0, FS, np.zeros(4 * 24000), 0.5, 24000)
        self.assertLess(al.peak_ratio, latency.AMBIGUOUS_RATIO)

    def test_silent_bis_reported(self):
        r = measure_latency.evaluate(synth_capture(0.0300, silent_tx=True), ARGS)
        self.assertTrue(r.channels[0].silent)
        self.assertTrue(r.channels[0].ambiguous)

    def test_lost_sdu_status_packets(self):
        r = measure_latency.evaluate(synth_capture(0.0300, lost_sdus=(0, 120, 121)), ARGS)
        self.assertEqual(r.channels[0].sdu_len, 60)
        self.assertEqual(r.channels[0].missing_sdus, 3)
        self.assertAlmostEqual(r.channels[0].i2s_to_arrival_s, 0.0325, delta=100e-6)

    def test_concealed_blocks_are_not_glitches(self):
        r = measure_latency.evaluate(synth_capture(0.0300, lost_sdus=(100, 250)), ARGS)
        self.assertEqual(r.channels[0].glitch_blocks, [])

    def test_restart_reported(self):
        r = measure_latency.evaluate(synth_capture(0.0300, restart_at=150), ARGS)
        self.assertEqual(r.channels[0].segments, 2)
        self.assertEqual(r.channels[0].discarded_sdus, 150)
        self.assertAlmostEqual(r.channels[0].i2s_to_arrival_s, 0.0325, delta=100e-6)

    def test_content_step_reported(self):
        r = measure_latency.evaluate(synth_capture(0.0300, content_step_at=200, content_step_s=0.004), ARGS)
        self.assertGreater(r.channels[0].lag_spread_s, 0.003)
        self.assertTrue(r.channels[0].unstable)

    def test_stable_latency_not_flagged(self):
        r = measure_latency.evaluate(synth_capture(0.0300), ARGS)
        self.assertLess(r.channels[0].lag_spread_s, 0.0002)
        self.assertFalse(r.channels[0].unstable)

    def test_rtp_timestamp_jump(self):
        r = measure_latency.evaluate(synth_capture(0.0300, ts_jump_at=2500), ARGS)
        self.assertAlmostEqual(r.channels[0].i2s_to_arrival_s, 0.0325, delta=100e-6)

    def test_cli_defaults_match_firmware_defaults(self):
        a = measure_latency.build_parser().parse_args([])
        self.assertEqual((a.pd_us, a.fifo_frames), (20000, 840))

    def test_no_iso_packets_message(self):
        only_rtp = [p for p in synth_capture(0.03, seconds=1) if p.linktype == 1]
        with self.assertRaises(SystemExit) as cm:
            measure_latency.evaluate(only_rtp, ARGS)
        self.assertIn("auracast_listen.py", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
