# SPDX-License-Identifier: Apache-2.0
"""Tests for capture.py (run: /usr/bin/python3 -m unittest discover -s tools -p 'test_*.py')."""

import os
import struct
import tempfile
import unittest

import numpy as np

import capture


def _block(btype, body):
    body += b"\0" * (-len(body) % 4)
    n = 12 + len(body)
    return struct.pack("<II", btype, n) + body + struct.pack("<I", n)


def make_pcapng(interfaces, packets):
    """interfaces: list of linktypes; packets: list of (if_id, t_seconds, data). Timestamps in us."""
    out = _block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))
    for lt in interfaces:
        out += _block(1, struct.pack("<HHI", lt, 0, 0))
    for if_id, t, data in packets:
        us = int(round(t * 1e6))
        out += _block(6, struct.pack("<IIIII", if_id, us >> 32, us & 0xFFFFFFFF, len(data), len(data)) + data)
    return out


def write_tmp(data):
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".pcapng")
    f.write(data)
    f.close()
    return f.name


def udp_frame(dst_ip, dport, payload, src_ip="192.168.178.85"):
    ip_src = bytes(map(int, src_ip.split(".")))
    ip_dst = bytes(map(int, dst_ip.split(".")))
    udp = struct.pack(">HHHH", 5004, dport, 8 + len(payload), 0) + payload
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 15, 17, 0, ip_src, ip_dst) + udp
    return b"\x01\x00\x5e\x45\x00\x01" + b"\x02" * 6 + b"\x08\x00" + ip


def rtp(seq, ts, payload=b"\0" * 288):
    return struct.pack(">BBHII", 0x80, 97, seq, ts, 0x2FED0954) + payload


def monitor(opcode, hci):
    return struct.pack(">HH", 0, opcode) + hci


def iso_rx(handle, seq, sdu, ts_us=None):
    hdr = b""
    flags = 0x2 << 12   # PB = complete SDU
    if ts_us is not None:
        flags |= 1 << 14
        hdr += struct.pack("<I", ts_us)
    hdr += struct.pack("<HH", seq, len(sdu))
    body = hdr + sdu
    return monitor(19, struct.pack("<HH", handle | flags, len(body)) + body)


class CaptureTest(unittest.TestCase):
    def tearDown(self):
        for p in getattr(self, "_files", []):
            os.unlink(p)

    def pcap(self, interfaces, packets):
        p = write_tmp(make_pcapng(interfaces, packets))
        self._files = getattr(self, "_files", []) + [p]
        return p

    def test_read_pcapng_two_interfaces(self):
        p = self.pcap([1, 254], [(0, 1000.000123, b"eth"), (1, 1000.000456, b"bt")])
        pk = list(capture.read_pcapng(p))
        self.assertEqual(len(pk), 2)
        self.assertEqual((pk[0].if_id, pk[0].linktype, pk[0].data), (0, 1, b"eth"))
        self.assertEqual((pk[1].if_id, pk[1].linktype, pk[1].data), (1, 254, b"bt"))
        self.assertAlmostEqual(pk[0].t, 1000.000123, delta=1e-6)
        self.assertAlmostEqual(pk[1].t, 1000.000456, delta=1e-6)

    def test_rtp_filter_and_fields(self):
        p = self.pcap([1], [
            (0, 1.0, udp_frame("239.69.0.1", 5004, rtp(10, 480))),
            (0, 1.001, udp_frame("239.69.5.1", 5004, rtp(99, 999))),
            (0, 1.002, udp_frame("239.69.0.1", 5004, rtp(11, 528))),
        ])
        r = capture.rtp_packets(capture.read_pcapng(p), "239.69.0.1")
        self.assertEqual([(x.seq, x.ts) for x in r], [(10, 480), (11, 528)])
        self.assertTrue(all(len(x.payload) == 288 for x in r))

    def test_rtp_ts_unwrap(self):
        p = self.pcap([1], [(0, 1.0 + i * 1e-3, udp_frame("239.69.0.1", 5004, rtp(i, ts)))
                            for i, ts in enumerate([0xFFFFFFD0, 0x00000000, 0x00000030])])
        r = capture.rtp_packets(capture.read_pcapng(p), "239.69.0.1")
        self.assertEqual([x.ts for x in r], [4294967248, 4294967296, 4294967344])

    def test_l24_decode(self):
        x = capture.l24_to_float(bytes.fromhex("7FFFFF800000000001FFFFFF"), 2)
        np.testing.assert_allclose(x, [[0.99999988, -1.0], [1.19e-7, -1.19e-7]], atol=1e-7)

    def test_hci_iso_rx_with_timestamp(self):
        p = self.pcap([254], [(0, 2.0, iso_rx(0x0010, 7, b"\x55" * 60, ts_us=123456))])
        s = capture.hci_iso_rx(capture.read_pcapng(p))
        self.assertEqual(len(s), 1)
        self.assertEqual((s[0].handle, s[0].seq, s[0].ts_us, len(s[0].sdu)), (0x10, 7, 123456, 60))
        self.assertAlmostEqual(s[0].t, 2.0, delta=1e-6)

    def test_big_sync_established(self):
        params = bytes([0x1D, 0x00, 0x00]) + (10000).to_bytes(3, "little") + bytes([4, 1, 0, 2]) \
            + struct.pack("<HHB", 60, 8, 1) + struct.pack("<H", 0x0010)
        evt = bytes([0x3E, len(params)]) + params
        p = self.pcap([254], [(0, 3.0, monitor(3, evt))])
        b = capture.big_sync_established(capture.read_pcapng(p))
        self.assertEqual(len(b), 1)
        self.assertEqual((b[0].big_handle, b[0].transport_latency_us, b[0].nse, b[0].bn, b[0].pto, b[0].irc,
                          b[0].iso_interval_us, b[0].bis_handles), (0, 10000, 4, 1, 0, 2, 10000, [0x10]))

    def test_longest_segment(self):
        sdus = [capture.IsoSdu(i * 0.01, 0x10, i, None, b"") for i in range(100)]
        sdus += [capture.IsoSdu(2.0 + i * 0.01, 0x10, i, None, b"") for i in range(300)]
        seg = capture.longest_segment(sdus, 0x10)
        self.assertEqual(len(seg), 300)
        self.assertEqual(seg[0].t, 2.0)


if __name__ == "__main__":
    unittest.main()
