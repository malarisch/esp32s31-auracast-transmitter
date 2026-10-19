# SPDX-License-Identifier: Apache-2.0
"""Minimal ctypes wrapper around the system liblc3 (float PCM, one channel per instance)."""

import ctypes

import numpy as np

_lib = ctypes.CDLL("liblc3.so.1")
_lib.lc3_decoder_size.argtypes = [ctypes.c_int, ctypes.c_int]
_lib.lc3_decoder_size.restype = ctypes.c_uint
_lib.lc3_setup_decoder.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
_lib.lc3_setup_decoder.restype = ctypes.c_void_p
_lib.lc3_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                            ctypes.c_int]
_lib.lc3_decode.restype = ctypes.c_int
_lib.lc3_encoder_size.argtypes = [ctypes.c_int, ctypes.c_int]
_lib.lc3_encoder_size.restype = ctypes.c_uint
_lib.lc3_setup_encoder.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
_lib.lc3_setup_encoder.restype = ctypes.c_void_p
_lib.lc3_encode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                            ctypes.c_void_p]
_lib.lc3_encode.restype = ctypes.c_int
_lib.lc3_delay_samples.argtypes = [ctypes.c_int, ctypes.c_int]
_lib.lc3_delay_samples.restype = ctypes.c_int

PCM_FLOAT = 3   # enum lc3_pcm_format: S16, S24, S24_3LE, FLOAT


def delay_samples(sample_rate: int, frame_us: int = 10000) -> int:
    """Algorithmic delay of encoder + decoder in samples."""
    return _lib.lc3_delay_samples(frame_us, sample_rate)


class Decoder:
    def __init__(self, sample_rate: int, frame_us: int = 10000):
        self.n = sample_rate * frame_us // 1000000
        self._mem = ctypes.create_string_buffer(_lib.lc3_decoder_size(frame_us, sample_rate))
        self._dec = _lib.lc3_setup_decoder(frame_us, sample_rate, sample_rate, self._mem)
        if not self._dec:
            raise ValueError(f"liblc3: unsupported decoder config {sample_rate} Hz / {frame_us} us")

    def decode(self, sdu) -> np.ndarray:
        """One frame of float32 PCM; sdu=None runs packet loss concealment."""
        out = np.zeros(self.n, dtype=np.float32)
        buf = None if sdu is None else ctypes.create_string_buffer(bytes(sdu), len(sdu))
        nbytes = 0 if sdu is None else len(sdu)
        if _lib.lc3_decode(self._dec, buf, nbytes, PCM_FLOAT, out.ctypes.data, 1) < 0:
            raise ValueError("liblc3: decode failed")
        return out


class Encoder:
    def __init__(self, sample_rate: int, frame_us: int = 10000):
        self.n = sample_rate * frame_us // 1000000
        self._mem = ctypes.create_string_buffer(_lib.lc3_encoder_size(frame_us, sample_rate))
        self._enc = _lib.lc3_setup_encoder(frame_us, sample_rate, sample_rate, self._mem)
        if not self._enc:
            raise ValueError(f"liblc3: unsupported encoder config {sample_rate} Hz / {frame_us} us")

    def encode(self, pcm: np.ndarray, nbytes: int) -> bytes:
        x = np.ascontiguousarray(pcm, dtype=np.float32)
        out = ctypes.create_string_buffer(nbytes)
        if _lib.lc3_encode(self._enc, PCM_FLOAT, x.ctypes.data, 1, nbytes, out) < 0:
            raise ValueError("liblc3: encode failed")
        return out.raw
