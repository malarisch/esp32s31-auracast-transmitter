# SPDX-License-Identifier: Apache-2.0
"""Parsers for captures taken with dumpcap on the AES67 interface and on bluetooth-monitor.

Only struct and numpy: pcapng (little endian) with Ethernet/IPv4/UDP/RTP on one side and the Linux
Bluetooth monitor (linktype 254) with HCI events and ISO data on the other.
"""

import socket
import struct
from typing import Iterator, NamedTuple, Optional

import numpy as np

LINKTYPE_ETHERNET = 1
LINKTYPE_BT_MONITOR = 254

MON_EVENT = 3      # btmon monitor/bt.h: BTSNOOP_OPCODE_EVENT_PKT
MON_ISO_RX = 19    # BTSNOOP_OPCODE_ISO_RX_PKT

EVT_LE_META = 0x3E
LE_BIG_SYNC_ESTABLISHED = 0x1D


class Packet(NamedTuple):
    if_id: int
    linktype: int
    t: float
    data: bytes


class RtpPkt(NamedTuple):
    t: float
    seq: int
    ts: int        # unwrapped, monotonic across 32-bit wraps
    payload: bytes


class IsoSdu(NamedTuple):
    t: float
    handle: int
    seq: int
    ts_us: Optional[int]
    sdu: bytes


class BigSync(NamedTuple):
    t: float
    big_handle: int
    transport_latency_us: int
    nse: int
    bn: int
    pto: int
    irc: int
    iso_interval_us: int
    bis_handles: list


def read_pcapng(path) -> Iterator[Packet]:
    with open(path, "rb") as f:
        data = f.read()
    ifaces = []   # (linktype, seconds per tick)
    i = 0
    while i + 12 <= len(data):
        btype, blen = struct.unpack_from("<II", data, i)
        if blen < 12 or i + blen > len(data):
            break
        body = data[i + 8:i + blen - 4]
        if btype == 0x0A0D0D0A:
            if struct.unpack_from("<I", body, 0)[0] != 0x1A2B3C4D:
                raise ValueError(f"{path}: only little-endian pcapng is supported")
            ifaces = []
        elif btype == 1:
            linktype = struct.unpack_from("<H", body, 0)[0]
            ifaces.append([linktype, _tsresol(body[8:])])
        elif btype == 6:
            if_id, hi, lo, caplen = struct.unpack_from("<IIII", body, 0)
            lt, res = ifaces[if_id]
            yield Packet(if_id, lt, ((hi << 32) | lo) * res, bytes(body[20:20 + caplen]))
        i += blen


def _tsresol(opts) -> float:
    j = 0
    while j + 4 <= len(opts):
        code, olen = struct.unpack_from("<HH", opts, j)
        if code == 0:
            break
        if code == 9 and olen >= 1:
            v = opts[j + 4]
            return 2.0 ** -(v & 0x7F) if v & 0x80 else 10.0 ** -v
        j += 4 + olen + (-olen % 4)
    return 1e-6


def _udp_payload(frame: bytes):
    """(dst_ip, dst_port, payload) of an IPv4/UDP Ethernet frame, else None."""
    off = 12
    etype = struct.unpack_from(">H", frame, off)[0]
    while etype == 0x8100:
        off += 4
        etype = struct.unpack_from(">H", frame, off)[0]
    off += 2
    if etype != 0x0800 or len(frame) < off + 20:
        return None
    ihl = (frame[off] & 0x0F) * 4
    if frame[off + 9] != 17:
        return None
    dst = socket.inet_ntoa(frame[off + 16:off + 20])
    u = off + ihl
    dport, ulen = struct.unpack_from(">HH", frame, u + 2)
    return dst, dport, frame[u + 8:u + ulen]


def rtp_packets(packets, group: str, port: int = 5004) -> list:
    out = []
    prev = None
    base = 0
    for p in packets:
        if p.linktype != LINKTYPE_ETHERNET:
            continue
        u = _udp_payload(p.data)
        if u is None or u[0] != group or u[1] != port or len(u[2]) < 12:
            continue
        pl = u[2]
        cc = pl[0] & 0x0F
        off = 12 + 4 * cc
        if pl[0] & 0x10:   # header extension
            off += 4 + 4 * struct.unpack_from(">H", pl, off + 2)[0]
        seq, ts = struct.unpack_from(">HI", pl, 2)
        if prev is not None and ts < prev and prev - ts > 0x80000000:
            base += 1 << 32
        prev = ts
        out.append(RtpPkt(p.t, seq, base + ts, pl[off:]))
    return out


def l24_to_float(payload: bytes, channels: int) -> np.ndarray:
    b = np.frombuffer(payload, dtype=np.uint8)
    b = b[:len(b) // (3 * channels) * 3 * channels].reshape(-1, 3).astype(np.int32)
    v = (b[:, 0] << 16) | (b[:, 1] << 8) | b[:, 2]
    v = np.where(v & 0x800000, v - 0x1000000, v)
    return (v / 8388608.0).reshape(-1, channels)


def _monitor(packets, opcode):
    for p in packets:
        if p.linktype == LINKTYPE_BT_MONITOR and len(p.data) >= 4:
            if struct.unpack_from(">H", p.data, 2)[0] == opcode:
                yield p.t, p.data[4:]


def hci_iso_rx(packets) -> list:
    """Complete ISO SDUs received by the host. Fragmented SDUs (PB 00/01/11) are reassembled."""
    out = []
    partial = {}
    for t, h in _monitor(packets, MON_ISO_RX):
        if len(h) < 4:
            continue
        hf, dlen = struct.unpack_from("<HH", h, 0)
        handle, pb, ts_flag = hf & 0x0FFF, (hf >> 12) & 3, (hf >> 14) & 1
        body = h[4:4 + (dlen & 0x3FFF)]
        if pb in (0, 2):   # first fragment or complete SDU: carries the ISO data load header
            off = 0
            ts_us = None
            if ts_flag:
                ts_us = struct.unpack_from("<I", body, 0)[0]
                off = 4
            seq, slen = struct.unpack_from("<HH", body, off)
            frag = (t, handle, seq, ts_us, slen & 0x0FFF, bytearray(body[off + 4:]))
            if pb == 2:
                out.append(IsoSdu(t, handle, seq, ts_us, bytes(frag[5])))
            else:
                partial[handle] = frag
        elif handle in partial:
            partial[handle][5].extend(body)
            if pb == 3:
                f = partial.pop(handle)
                out.append(IsoSdu(t, handle, f[2], f[3], bytes(f[5])))
    return out


def big_sync_established(packets) -> list:
    out = []
    for t, h in _monitor(packets, MON_EVENT):
        if len(h) < 3 or h[0] != EVT_LE_META or h[2] != LE_BIG_SYNC_ESTABLISHED:
            continue
        e = h[2:]
        if e[1] != 0:   # status
            continue
        big = e[2]
        tl = int.from_bytes(e[3:6], "little")
        nse, bn, pto, irc = e[6:10]
        _max_pdu, iso_int, n = struct.unpack_from("<HHB", e, 10)
        handles = [struct.unpack_from("<H", e, 15 + 2 * k)[0] for k in range(n)]
        out.append(BigSync(t, big, tl, nse, bn, pto, irc, iso_int * 1250, handles))
    return out


def longest_segment(sdus, handle: int, max_gap: int = 50) -> list:
    """Longest run of SDUs on `handle` whose sequence numbers increase by 1..max_gap (mod 2^16)."""
    best, cur = [], []
    for s in (x for x in sdus if x.handle == handle):
        if cur and not (1 <= (s.seq - cur[-1].seq) % 65536 <= max_gap):
            if len(cur) > len(best):
                best = cur
            cur = []
        cur.append(s)
    return cur if len(cur) > len(best) else best
