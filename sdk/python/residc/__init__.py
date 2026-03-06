"""residc — Per-message prediction-residual compression for financial data.

Usage:
    from residc import Codec, TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL

    enc = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])
    dec = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])

    data = enc.encode([34200000000000, 42, 1500250, 100, 0])
    values = dec.decode(data)
    assert values == [34200000000000, 42, 1500250, 100, 0]
"""

from __future__ import annotations

import ctypes
import os
import platform
from typing import Optional

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


_VALID_FIELD_TYPES = frozenset(range(12))

_FIELD_TYPE_NAMES = {
    0: "TIMESTAMP", 1: "INSTRUMENT", 2: "PRICE", 3: "QUANTITY",
    4: "SEQUENTIAL_ID", 5: "ENUM", 6: "BOOL", 7: "CATEGORICAL",
    8: "RAW", 9: "DELTA_ID", 10: "DELTA_PRICE", 11: "COMPUTED",
}


class Snapshot:
    """Opaque wrapper around a residc codec state snapshot.

    Automatically frees the underlying C pointer when garbage collected.
    Can also be freed explicitly via Codec.free_snapshot().
    """

    def __init__(self, ptr: ctypes.c_void_p) -> None:
        self._ptr = ptr

    def __del__(self) -> None:
        if self._ptr:
            _lib.residc_codec_destroy(self._ptr)
            self._ptr = None


class Codec:
    """residc codec for encoding/decoding financial messages."""

    def __init__(self, fields: list[int], ref_fields: Optional[list[int]] = None) -> None:
        """Create a codec.

        Args:
            fields: List of field types (TIMESTAMP, INSTRUMENT, PRICE, etc.)
            ref_fields: Optional list of reference field indices for DELTA_*
                        fields. Use -1 for non-delta fields.

        Raises:
            ValueError: If fields is empty or contains invalid field types.
        """
        if not fields:
            raise ValueError("fields must be non-empty")

        invalid = [f for f in fields if f not in _VALID_FIELD_TYPES]
        if invalid:
            raise ValueError(
                f"invalid field type(s): {invalid}; "
                f"valid types are 0-11 ({', '.join(_FIELD_TYPE_NAMES[i] for i in sorted(_VALID_FIELD_TYPES))})"
            )

        n = len(fields)
        types_arr = (ctypes.c_int * n)(*fields)

        if ref_fields is not None:
            refs_arr = (ctypes.c_int8 * n)(*ref_fields)
            self._ptr = _lib.residc_codec_create(types_arr, refs_arr, n)
        else:
            self._ptr = _lib.residc_codec_create(types_arr, None, n)

        if not self._ptr:
            field_names = [_FIELD_TYPE_NAMES.get(f, str(f)) for f in fields]
            raise RuntimeError(
                f"Failed to create residc codec with {n} field(s): [{', '.join(field_names)}]"
            )

        self._num_fields = n
        self._values_type = ctypes.c_uint64 * n
        self._buf = (ctypes.c_uint8 * 256)()

    def __del__(self) -> None:
        if hasattr(self, "_ptr") and self._ptr:
            _lib.residc_codec_destroy(self._ptr)
            self._ptr = None

    def __enter__(self) -> Codec:
        return self

    def __exit__(self, *args: object) -> None:
        if self._ptr:
            _lib.residc_codec_destroy(self._ptr)
            self._ptr = None

    def encode(self, values: list[int]) -> bytes:
        """Encode a message. Returns compressed bytes.

        Raises:
            ValueError: If the number of values does not match the field count.
        """
        if len(values) != self._num_fields:
            raise ValueError(
                f"expected {self._num_fields} values, got {len(values)}"
            )
        vals = self._values_type(*values)
        length = _lib.residc_codec_encode(self._ptr, vals, self._buf, 256)
        if length < 0:
            raise RuntimeError("residc encode failed")
        return bytes(self._buf[:length])

    def decode(self, data: bytes) -> list[int]:
        """Decode a message. Returns list of field values.

        Raises:
            ValueError: If data is not bytes or is empty.
        """
        if not isinstance(data, bytes):
            raise ValueError(f"data must be bytes, got {type(data).__name__}")
        if not data:
            raise ValueError("data must be non-empty")
        in_buf = (ctypes.c_uint8 * len(data))(*data)
        vals = self._values_type()
        consumed = _lib.residc_codec_decode(
            self._ptr, in_buf, len(data), vals
        )
        if consumed < 0:
            raise RuntimeError("residc decode failed")
        return list(vals)

    def snapshot(self) -> Snapshot:
        """Take a state snapshot for gap recovery. Returns a Snapshot object."""
        snap = _lib.residc_codec_snapshot(self._ptr)
        if not snap:
            raise RuntimeError("residc snapshot failed")
        return Snapshot(snap)

    def restore(self, snapshot: Snapshot) -> None:
        """Restore state from a snapshot."""
        _lib.residc_codec_restore(self._ptr, snapshot._ptr)

    def free_snapshot(self, snapshot: Snapshot) -> None:
        """Free a previously taken snapshot.

        Note: Snapshots are automatically freed when garbage collected,
        so calling this is optional but allows deterministic cleanup.
        """
        if snapshot._ptr:
            _lib.residc_codec_destroy(snapshot._ptr)
            snapshot._ptr = None

    def seed_mfu(self, instruments: list[tuple[int, int]]) -> None:
        """Pre-seed instrument frequency table.

        Args:
            instruments: List of (id, count) tuples sorted by frequency.
                         Both id and count are uint16 (max 65535).
        """
        n = len(instruments)
        ids = (ctypes.c_uint16 * n)(*(i for i, _ in instruments))
        counts = (ctypes.c_uint16 * n)(*(c for _, c in instruments))
        _lib.residc_codec_seed_mfu(self._ptr, ids, counts, n)

    def reset(self) -> None:
        """Reset codec to initial state."""
        _lib.residc_codec_reset(self._ptr)
