"""residc — Per-message prediction-residual compression for financial data.

Usage:
    from residc import Codec, TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL

    enc = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])
    dec = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])

    data = enc.encode([34200000000000, 42, 1500250, 100, 0])
    values = dec.decode(data)
    assert values == [34200000000000, 42, 1500250, 100, 0]
"""

import ctypes
import os
import platform

# Field types
TIMESTAMP = 0
INSTRUMENT = 1
PRICE = 2
QUANTITY = 3
SEQUENTIAL_ID = 4
ENUM = 5
BOOL = 6
CATEGORICAL = 7
RAW = 8
DELTA_ID = 9
DELTA_PRICE = 10
COMPUTED = 11


def _load_lib():
    here = os.path.dirname(os.path.abspath(__file__))
    system = platform.system()
    if system == "Darwin":
        name = "libresdc.dylib"
    elif system == "Windows":
        name = "residc.dll"
    else:
        name = "libresdc.so"

    for search_dir in [here, os.path.join(here, ".."), os.path.join(here, "..", "..")]:
        path = os.path.join(search_dir, name)
        if os.path.exists(path):
            return ctypes.CDLL(path)

    return ctypes.CDLL(name)


_lib = _load_lib()

# C function signatures
_lib.residc_codec_create.restype = ctypes.c_void_p
_lib.residc_codec_create.argtypes = [
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int8),
    ctypes.c_int,
]

_lib.residc_codec_destroy.restype = None
_lib.residc_codec_destroy.argtypes = [ctypes.c_void_p]

_lib.residc_codec_encode.restype = ctypes.c_int
_lib.residc_codec_encode.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint64),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_int,
]

_lib.residc_codec_decode.restype = ctypes.c_int
_lib.residc_codec_decode.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_uint64),
]

_lib.residc_codec_snapshot.restype = ctypes.c_void_p
_lib.residc_codec_snapshot.argtypes = [ctypes.c_void_p]

_lib.residc_codec_restore.restype = None
_lib.residc_codec_restore.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

_lib.residc_codec_reset.restype = None
_lib.residc_codec_reset.argtypes = [ctypes.c_void_p]

_lib.residc_codec_seed_mfu.restype = None
_lib.residc_codec_seed_mfu.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_uint16),
    ctypes.POINTER(ctypes.c_uint16),
    ctypes.c_int,
]


class Codec:
    """residc codec for encoding/decoding financial messages."""

    def __init__(self, fields, ref_fields=None):
        """Create a codec.

        Args:
            fields: List of field types (TIMESTAMP, INSTRUMENT, PRICE, etc.)
            ref_fields: Optional list of reference field indices for DELTA_*
                        fields. Use -1 for non-delta fields.
        """
        n = len(fields)
        types_arr = (ctypes.c_int * n)(*fields)

        if ref_fields is not None:
            refs_arr = (ctypes.c_int8 * n)(*ref_fields)
            self._ptr = _lib.residc_codec_create(types_arr, refs_arr, n)
        else:
            self._ptr = _lib.residc_codec_create(types_arr, None, n)

        if not self._ptr:
            raise RuntimeError("Failed to create residc codec")

        self._num_fields = n
        self._values_type = ctypes.c_uint64 * n
        self._buf = (ctypes.c_uint8 * 256)()

    def __del__(self):
        if hasattr(self, "_ptr") and self._ptr:
            _lib.residc_codec_destroy(self._ptr)
            self._ptr = None

    def encode(self, values):
        """Encode a message. Returns compressed bytes."""
        vals = self._values_type(*values)
        length = _lib.residc_codec_encode(self._ptr, vals, self._buf, 256)
        if length < 0:
            raise RuntimeError("residc encode failed")
        return bytes(self._buf[:length])

    def decode(self, data):
        """Decode a message. Returns list of field values."""
        in_buf = (ctypes.c_uint8 * len(data))(*data)
        vals = self._values_type()
        consumed = _lib.residc_codec_decode(
            self._ptr, in_buf, len(data), vals
        )
        if consumed < 0:
            raise RuntimeError("residc decode failed")
        return list(vals)

    def snapshot(self):
        """Take a state snapshot for gap recovery. Returns opaque handle."""
        snap = _lib.residc_codec_snapshot(self._ptr)
        if not snap:
            raise RuntimeError("residc snapshot failed")
        return snap

    def restore(self, snapshot):
        """Restore state from a snapshot."""
        _lib.residc_codec_restore(self._ptr, snapshot)

    def free_snapshot(self, snapshot):
        """Free a previously taken snapshot."""
        _lib.residc_codec_destroy(snapshot)

    def seed_mfu(self, instruments):
        """Pre-seed instrument frequency table.

        Args:
            instruments: List of (id, count) tuples sorted by frequency.
        """
        n = len(instruments)
        ids = (ctypes.c_uint16 * n)(*(i for i, _ in instruments))
        counts = (ctypes.c_uint16 * n)(*(c for _, c in instruments))
        _lib.residc_codec_seed_mfu(self._ptr, ids, counts, n)

    def reset(self):
        """Reset codec to initial state."""
        _lib.residc_codec_reset(self._ptr)
