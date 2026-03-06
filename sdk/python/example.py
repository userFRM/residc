#!/usr/bin/env python3
"""Example: encode and decode financial quotes with residc."""

from residc import Codec, TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL

# Create encoder and decoder with the same schema
enc = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])
dec = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])

# Simulate a stream of quotes
quotes = [
    [34_200_000_000_000, 42, 1_500_250, 100, 0],
    [34_200_000_010_000, 42, 1_500_300, 200, 1],
    [34_200_000_020_000, 99, 1_200_000, 100, 0],
    [34_200_000_030_000, 42, 1_500_275, 100, 1],
    [34_200_000_040_000, 99, 1_200_100, 300, 0],
]

total_raw = 0
total_compressed = 0

print("residc Python SDK example")
print("=" * 60)

for q in quotes:
    data = enc.encode(q)
    decoded = dec.decode(data)

    raw_size = 8 + 2 + 4 + 4 + 1  # 19 bytes
    total_raw += raw_size
    total_compressed += len(data)

    assert decoded == q, f"Roundtrip error: {q} != {decoded}"
    print(
        f"  ts={q[0]:>15d}  inst={q[1]:>3d}  price={q[2]:>8d}  "
        f"qty={q[3]:>4d}  side={q[4]}  -> {len(data)} bytes"
    )

ratio = total_raw / total_compressed
print(f"\nRaw: {total_raw} bytes, Compressed: {total_compressed} bytes, "
      f"Ratio: {ratio:.2f}:1")
print("All 5 roundtrips verified.")
