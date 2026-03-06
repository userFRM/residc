# Wire Format Specification (C Implementation)

This documents the canonical wire format produced by the C implementation (`core/residc.c`). The Rust implementation uses a different tiered coding scheme and is **not** wire-compatible.

## Frame Structure

Each compressed message is a self-contained frame:

```
[1-byte frame header][payload]
```

- **Frame header 0x01..0xFE**: Compressed payload of that many bytes follows.
- **Frame header 0xFF (literal fallback)**: Raw field values follow in schema order (big-endian, field-size bytes each). Used when compression does not reduce size.

Compressed frames are capped at 254 bytes of payload. If the compressed output exceeds the raw message size or 254 bytes, the literal fallback is emitted instead.

## Field Encoding Order

Fields are encoded in schema order. The Instrument field (if present) must appear before any Price, Quantity, or SequentialId fields that depend on per-instrument state.

## Zigzag Encoding

Signed residuals are mapped to unsigned values before tiered coding:

```
zigzag_enc(v) = (v << 1) ^ (v >> 63)
zigzag_dec(u) = (u >> 1) ^ -(u & 1)
```

This maps `0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, ...` so that small-magnitude values produce small unsigned values.

## Tiered Residual Coding

The core coding primitive. Encodes a zigzag-encoded unsigned value `zz` using a parameter `k` that controls tier boundaries:

| Tier | Prefix | Payload bits | Value range | Total bits |
|------|--------|-------------|-------------|------------|
| 0 | `0` | k | `0 .. 2^k - 1` | 1 + k |
| 1 | `10` | k + 2 | `0 .. 2^(k+2) - 1` | 2 + k + 2 |
| 2 | `110` | k + 5 | `0 .. 2^(k+5) - 1` | 3 + k + 5 |
| 3 | `1110` | k + 10 | `0 .. 2^(k+10) - 1` | 4 + k + 10 |
| 4 | `1111` | 64 (raw) | any 64-bit value | 4 + 64 |

The decoder reads prefix bits one at a time until it finds a `0` bit (tiers 0-3) or reads `1111` (tier 4). For tier 4, the raw 64-bit signed value is written directly (not zigzag-encoded), split as two 32-bit writes (high then low).

The `k` parameter is regime-dependent:

| Field type | Calm k | Volatile k |
|-----------|--------|-----------|
| Timestamp | 10 | 8 |
| Price | 3 | 7 |
| Quantity | 4 | 7 |
| SequentialId | 3 | 5 |

## Adaptive k (Timestamp, Price, Quantity, SequentialId)

For Timestamp, Price, Quantity, and SequentialId fields, `k` is refined by an adaptive algorithm:

1. Maintain a running `sum` and `count` of recent zigzag-encoded residuals.
2. Compute `k = floor(log2(sum)) - floor(log2(count))`, clamped to `[k_min, k_max]` where `k_min` is the regime-dependent k and `k_max = k_min + 10`.
3. After each residual, update: `sum += zz; count++`. When `count >= 8`, halve both (`sum >>= 1; count >>= 1`).

For Price, the adaptive state tracks the coded residual (penny-normalized when applicable). For Quantity, adaptive state is updated only when the residual is non-zero (the "same as predicted" fast path does not update adaptive state).

## Per-Field Encoding

### Timestamp

1. Compute `gap = timestamp - last_timestamp`.
2. Compute `predicted_gap = max(0, timestamp_gap_ema >> 16)` (Q16 fixed-point EMA).
3. Compute `residual = gap - predicted_gap`.
4. Encode residual with adaptive k (base from regime).
5. Update EMA: `ema += ((gap << 16) - ema) >> 2`.

### Instrument

1. If same as last instrument and `msg_count > 0`: write `0` (1 bit).
2. Else write `1` (1 bit), then:
   - If found in MFU table at index `idx`: convert `idx` to frequency rank, then write `0` (1 bit) + variable-length rank encoding:
     - Rank 0-3: `0` (1 bit) + rank (2 bits) = 3 bits total
     - Rank 4-15: `10` (2 bits) + (rank-4) (4 bits) = 6 bits total
     - Rank 16-63: `110` (3 bits) + (rank-16) (6 bits) = 9 bits total
     - Rank 64-255: `111` (3 bits) + rank (8 bits) = 11 bits total
   - Else: write `1` (1 bit) + raw instrument ID (14 bits).

### Price

1. Predict from per-instrument last price (0 if no history).
2. Compute `residual = price - predicted`.
3. Penny normalization: if both price and predicted are multiples of 100 (and at least one is nonzero):
   - Write `0` (1 bit), encode `residual / 100`.
4. Else:
   - Write `1` (1 bit), encode raw residual.
5. Track regime: accumulate `|residual|` over 64 Price fields. If average > 30, switch to Volatile; otherwise Calm.

### Quantity

1. Predict from per-instrument last quantity (default 100).
2. If `residual == 0`: write `0` (1 bit).
3. Else write `1` (1 bit), then round-lot normalization:
   - If both qty and predicted are multiples of 100: write `0` (1 bit), encode `residual / 100`.
   - Else: write `1` (1 bit), encode raw residual.

### SequentialId

1. Predict from per-instrument last seq ID if available, otherwise global last value.
2. Encode `delta = id - predicted` with adaptive k.

### Enum

1. If same as last value and `msg_count > 0`: write `0` (1 bit).
2. Else: write `1` (1 bit) + raw value (`size * 8` bits).

### Bool

1. Write value as 1 bit.

### Categorical

1. If same as last value and `msg_count > 0`: write `0` (1 bit).
2. Else: write `1` (1 bit) + high 16 bits + low 16 bits.

### Raw

1. Write raw value (`size * 8` bits). For fields > 8 bytes, write byte-by-byte.

### DeltaId

1. Compute `delta = value - ref_field_value`.
2. Encode with tiered coding, k from SequentialId regime.

### DeltaPrice

1. Compute `delta = value - ref_field_value`.
2. Encode with tiered coding, k from Price regime.

### Computed

Not transmitted (0 bits on wire).

## MFU Table

The MFU (Most Frequently Used) table tracks the top 256 instruments by frequency.

- **Hash function**: `h = (uint8_t)(id * 157)` -- maps instrument ID to a bucket in a 256-entry hash table.
- **Structure**: 256 entries, each with `(instrument_id, count)`. Hash table with separate chaining for O(1) lookup.
- **Update**: On each instrument occurrence, increment count. If table is full and instrument is new, replace the entry with minimum count.
- **Decay**: Every 10,000 messages, all counts are halved (`count >>= 1`) to adapt to changing distributions.
- **Rank mapping**: Entries are ranked by descending frequency. Ranks are rebuilt at init, seed, and every decay cycle. Both encoder and decoder maintain identical rank mappings.
- **Encoding**: MFU index is encoded as a variable-length rank code:
  - Rank 0-3: prefix `0` + 2-bit rank = 3 bits total
  - Rank 4-15: prefix `10` + 4-bit offset (rank-4) = 6 bits total
  - Rank 16-63: prefix `110` + 6-bit offset (rank-16) = 9 bits total
  - Rank 64-255: prefix `111` + 8-bit offset (rank-64) = 11 bits total
- A non-MFU instrument uses the 14-bit raw escape code.

## State Evolution

Both encoder and decoder maintain identical state that evolves deterministically:

1. After encoding/decoding each message, `commit_state` updates:
   - Timestamp EMA and adaptive k window
   - MFU table (update + periodic decay)
   - `last_instrument_id`
   - Per-instrument `last_price`, `last_qty`, `msg_count`, `last_seq_id`
   - Per-field `last_value` (for Enum, Categorical, DeltaId, SequentialId)
   - Regime detection (price residual accumulation)
   - Global `msg_count`

2. State synchronization requires that encoder and decoder process the exact same message stream. If messages are lost, state diverges and subsequent decoding fails. Use checkpoint/restore for gap recovery.

## Multi-Type Messages

For protocols with multiple message types, a type prefix is encoded before the fields:

1. If same type as last message and `msg_count > 0`: write `0` (1 bit).
2. Else: write `1` (1 bit) + Huffman-like prefix code:
   - Type index 0: `0` (1 bit)
   - Type index 1: `10` (2 bits)
   - Type index N-2: `1...10` (N-1 bits)
   - Type index N-1: `1...1` (N-1 bits)

Types should be ordered by frequency (most frequent = index 0).
