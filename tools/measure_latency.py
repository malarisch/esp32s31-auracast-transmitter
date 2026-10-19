#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Latency of the Auracast bridge: sample at the ESP's I2S input -> SDU received by the PC's controller.

Captures the AES67 stream (the one the FPGA plays out to the ESP) and the Bluetooth monitor with one
dumpcap, so both sides share the PC clock, decodes L24 and LC3 and aligns them by cross-correlation.
PipeWire is not in the measured path.

  tools/measure_latency.py --seconds 20 --pd-us 40000            # broadcast must be subscribed
  tools/measure_latency.py --subscribe --seconds 20 --pd-us 20000  # starts auracast_listen.py itself
  tools/measure_latency.py --pcap capture.pcapng --pd-us 20000     # evaluate an existing capture

Timing chain (Mala, 2026-10-19): RTP timestamp = capture time of the packet's first sample; the sender
puts a packet on the wire ~29 us after its 48th sample, the SG350 adds ~12 us, so
capture(ts) = lower envelope of the arrivals - 1.041 ms. The FPGA at the ESP plays sample ts out on
I2S 96 samples (2 ms) after capture.
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time
from typing import NamedTuple, Optional

import numpy as np

import capture
import latency
import lc3

PATH_DELAY_S = 1.041e-3      # 1 ms packet time + 29 us sender + 12 us switch
PLAYOUT_S = 96 / 48000       # FPGA at the ESP: capture -> I2S
SDU_RATE = {40: 16000, 60: 24000, 100: 48000, 120: 48000, 155: 48000}
HERE = os.path.dirname(os.path.abspath(__file__))


class ChannelResult(NamedTuple):
    handle: int
    source: str            # reference channel the BIS matched best: "L", "R" or "L+R"
    sample_rate: int
    sdu_len: int
    i2s_to_arrival_s: float
    peak_ratio: float
    ambiguous: bool
    gain: float
    sdus: int
    missing_sdus: int
    glitch_blocks: list
    blocks: int


class Report(NamedTuple):
    channels: list
    rtp_packets: int
    missing_rtp: int
    transport_latency_us: Optional[int]
    pd_us: int
    fifo_frames: int


def _reference(rtp, channels):
    ts = np.array([p.ts for p in rtp], dtype=np.int64)
    fpp = len(rtp[0].payload) // (3 * channels)
    n = int(ts[-1] - ts[0]) + fpp
    ref = np.zeros((n, channels))
    for p in rtp:
        x = capture.l24_to_float(p.payload, channels)
        i = int(p.ts - ts[0])
        ref[i:i + len(x)] = x
    missing = int(np.sum(np.maximum(np.diff(ts) // fpp - 1, 0)))
    return ref, missing


CONFIRM_DB = 6.0
SILENT_DB = -50.0


def glitches(ref_aligned, rx, fs, sdu_len) -> list:
    """Disturbed 10 ms blocks.

    Candidates come from latency.glitch_blocks() (a 1 ms sub-block with SNR < 1 dB and an error far above
    the median). A candidate counts only if, in its worst sub-block, the received audio has at least
    CONFIRM_DB more error than liblc3 achieves on the same reference with the same frame size: passages
    that are simply hard for LC3 at this bit rate are not clicks. The first block is skipped (a cold
    decoder starts with ~2.5 ms of silence)."""
    fs = int(round(fs))
    n, sub, d = fs // 100, fs // 1000, lc3.delay_samples(fs)
    ref_aligned = np.asarray(ref_aligned, dtype=np.float64)
    cand = [int(k) + 1 for k in latency.glitch_blocks(ref_aligned[n:], rx[n:], fs)]
    if not cand:
        return []
    # ref_aligned already includes the codec delay; undo it so our frames match the transmitter's
    src = np.concatenate([ref_aligned[d:], np.zeros(d)]).astype(np.float32)
    enc, dec = lc3.Encoder(fs), lc3.Decoder(fs)
    last = max(cand) + 1
    ideal = np.concatenate([dec.decode(enc.encode(src[k * n:(k + 1) * n], sdu_len)) for k in range(last)])
    out = []
    for k in cand:
        worst = -np.inf
        for j in range(k * n, (k + 1) * n - sub + 1, sub):
            r = ref_aligned[j:j + sub]
            if np.mean(r ** 2) < 10 ** (SILENT_DB / 10):
                continue
            e_rx = np.mean((rx[j:j + sub] - r) ** 2)
            e_id = max(np.mean((ideal[j:j + sub] - r) ** 2), 1e-20)
            worst = max(worst, 10 * np.log10(max(e_rx, 1e-20) / e_id))
        if worst >= CONFIRM_DB:
            out.append(k)
    return out


def _sources(ref):
    if ref.shape[1] == 1:
        return [("L+R", ref[:, 0])]
    return [("L", ref[:, 0]), ("R", ref[:, 1]), ("L+R", ref.mean(axis=1))]


def _decode(seg, sample_rate):
    dec = lc3.Decoder(sample_rate)
    seq = np.array([s.seq for s in seg], dtype=np.int64)
    seq = seq[0] + np.concatenate([[0], np.cumsum(np.diff(seq) % 65536)])
    out, missing = [], 0
    prev = None
    for s, q in zip(seg, seq):
        if prev is not None:
            for _ in range(int(q - prev - 1)):
                out.append(dec.decode(None))
                missing += 1
        out.append(dec.decode(s.sdu))
        prev = q
    return np.concatenate(out), seq, missing


def evaluate(packets, args) -> Report:
    packets = list(packets)
    rtp = capture.rtp_packets(packets, args.group, args.port)
    if len(rtp) < 100:
        raise SystemExit(f"Zu wenige RTP-Pakete an {args.group}:{args.port} im Mitschnitt ({len(rtp)}).")
    sdus = capture.hci_iso_rx(packets)
    if not sdus:
        raise SystemExit("Keine ISO-SDUs im Mitschnitt. Ist der Broadcast abonniert? "
                         "(tools/auracast_listen.py oder --subscribe)")

    ref, missing_rtp = _reference(rtp, args.channels)
    a, b = latency.capture_time_fit([p.ts for p in rtp], [p.t for p in rtp], PATH_DELAY_S)
    ref_t0 = a + b * rtp[0].ts + b * 96      # I2S time of ref[0]: capture + 96 samples
    ref_fs = 1.0 / b

    handles = sorted({s.handle for s in sdus})
    results = []
    for idx, h in enumerate(handles):
        seg = capture.longest_segment(sdus, h)
        sdu_len = len(seg[0].sdu)
        fs = SDU_RATE.get(sdu_len)
        if fs is None:
            raise SystemExit(f"Unbekannte SDU-Länge {sdu_len} B auf Handle 0x{h:03x}.")
        rx, seq, missing = _decode(seg, fs)
        a2, b2 = latency.arrival_fit(seq, [s.t for s in seg])
        rx_t0 = a2 + b2 * seq[0]
        rx_fs = (fs // 100) / b2
        # Align against every candidate source and keep the one most similar at its lag: reveals swapped channels
        best = None
        for name, r in _sources(ref):
            al = latency.align(r, ref_t0, ref_fs, rx, rx_t0, rx_fs)
            ra = latency.aligned_ref(r, ref_t0, ref_fs, rx_t0, rx_fs, len(rx), al.lag_s)
            nc = float(np.dot(rx, ra) / max(np.sqrt(np.dot(rx, rx) * np.dot(ra, ra)), 1e-20))
            if best is None or nc > best[3]:
                best = (name, ra, al, nc)
        name, ra, al, _ = best
        gl = glitches(ra * al.gain, rx, fs, sdu_len)
        results.append(ChannelResult(h, name, fs, sdu_len, al.lag_s, al.peak_ratio,
                                     al.peak_ratio < latency.AMBIGUOUS_RATIO, al.gain, len(seg), missing,
                                     gl, len(rx) // (fs // 100)))

    tl = getattr(args, "transport_latency_us", None)
    syncs = [s for s in capture.big_sync_established(packets) if set(s.bis_handles) & set(handles)]
    if syncs:
        tl = syncs[-1].transport_latency_us
    return Report(results, len(rtp), missing_rtp, tl, args.pd_us, args.fifo_frames)


def print_report(r: Report):
    ms = 1e3
    print(f"RTP: {r.rtp_packets} Pakete, {r.missing_rtp} fehlend")
    for c in r.channels:
        warn = "  MEHRDEUTIG (periodischer Inhalt?)" if c.ambiguous else ""
        print(f"BIS 0x{c.handle:03x} = {c.source}: {c.sample_rate} Hz, {c.sdu_len} B, {c.sdus} SDUs, {c.missing_sdus} fehlend, "
              f"Korrelation {c.peak_ratio:.1f}x, Pegel {20 * np.log10(max(c.gain, 1e-9)):+.1f} dB{warn}")
        print(f"  gestörte 10-ms-Blöcke: {len(c.glitch_blocks)} von {c.blocks}"
              + (f" (bei {', '.join(f'{g / 100:.2f} s' for g in c.glitch_blocks[:10])})" if c.glitch_blocks else ""))
    print()
    for c in r.channels:
        print(f"BIS 0x{c.handle:03x}: I2S-Eingang ESP -> SDU am PC : {c.i2s_to_arrival_s * ms:7.2f} ms  "
              f"(AES67-Erfassung -> SDU: {(c.i2s_to_arrival_s + PLAYOUT_S) * ms:.2f} ms, inkl. 2,5 ms LC3-Algorithmus)")
    # All BIS of a BIG share one synchronisation reference: rendering follows the latest one
    lag = max(c.i2s_to_arrival_s for c in r.channels)
    lo = lag + r.pd_us * 1e-6
    if r.transport_latency_us is not None:
        hi = lo + r.transport_latency_us * 1e-6
        print(f"I2S-Eingang ESP  -> gerendert (Hörgerät) : {lo * ms:7.2f} .. {hi * ms:.2f} ms  "
              f"(PD {r.pd_us / 1000:.0f} ms, BIG-Transportlatenz {r.transport_latency_us / 1000:.1f} ms)")
    else:
        print(f"I2S-Eingang ESP  -> gerendert (Hörgerät) : >= {lo * ms:6.2f} ms  (PD {r.pd_us / 1000:.0f} ms; "
              f"Obergrenze unbekannt: BIG Sync nicht im Mitschnitt, --subscribe oder --transport-latency-us)")
    print("  Immediate Rendering kann gerätespezifisch früher rendern.")
    print(f"Anteile: FIFO-Sollwert {r.fifo_frames / 48:.1f} ms, LC3-Frame 10 ms, LC3-Algorithmus 2,5 ms")
    if len(r.channels) > 1:
        if [c.source for c in r.channels[:2]] != ["L", "R"]:
            print(f"ACHTUNG: BIS 1/2 tragen {r.channels[0].source}/{r.channels[1].source} statt L/R")


def run_capture(args) -> str:
    out_dir = args.out or tempfile.mkdtemp(prefix="latency_")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"latency_{time.strftime('%Y%m%d_%H%M%S')}.pcapng")
    dump = subprocess.Popen(["dumpcap", "-q", "-i", args.iface, "-f",
                             f"udp and dst host {args.group} and port {args.port}",
                             "-i", "bluetooth-monitor", "-w", path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    listener = None
    try:
        time.sleep(1.0)
        if args.subscribe:
            listener = subprocess.Popen([sys.executable, os.path.join(HERE, "auracast_listen.py")],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            t_end = time.time() + 90
            for line in listener.stdout:
                print("  listen:", line.rstrip())
                if "receiving" in line or time.time() > t_end:
                    break
        print(f"Mitschnitt läuft {args.seconds} s ...", flush=True)
        time.sleep(args.seconds)
    finally:
        if listener:
            listener.send_signal(signal.SIGINT)
            listener.wait(timeout=10)
        dump.send_signal(signal.SIGINT)
        dump.wait(timeout=10)
    print(f"Mitschnitt: {path}")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--pcap", help="evaluate an existing capture instead of capturing")
    ap.add_argument("--subscribe", action="store_true", help="start auracast_listen.py inside the capture")
    ap.add_argument("--iface", default="enp2s0")
    ap.add_argument("--group", default="239.69.0.1")
    ap.add_argument("--port", type=int, default=5004)
    ap.add_argument("--channels", type=int, default=2, help="channels in the AES67 stream")
    ap.add_argument("--pd-us", type=int, default=40000, help="presentation delay announced by the bridge")
    ap.add_argument("--fifo-frames", type=int, default=960, help="AURACAST_FIFO_TARGET_FRAMES of the firmware")
    ap.add_argument("--transport-latency-us", type=int, help="BIG transport latency if not in the capture")
    ap.add_argument("--out", help="directory for the capture file")
    args = ap.parse_args()

    path = args.pcap or run_capture(args)
    print_report(evaluate(capture.read_pcapng(path), args))


if __name__ == "__main__":
    main()
