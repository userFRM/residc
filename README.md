# residc

**Schema-driven, per-message prediction-residual compression for financial data.**

residc compresses financial messages (quotes, orders, trades) **~2x** by predicting each field from context and encoding only the prediction error. Unlike general-purpose compressors (LZ4, zstd), it works on individual messages -- no blocks, no buffering, per-message random access.

Structure-agnostic: you define any message struct, map fields to prediction types, and the codec handles the rest.

## Benchmarks

100K synthetic messages per type, gcc -O2, `-march=native`, best of 10 iterations. Reproducible: `cc -O2 -march=native -o bench_compression bench/bench_compression.c core/residc.c -Icore && ./bench_compression`

| Message | Fields | Raw | Compressed | Ratio | Encode | Decode |
|---------|--------|-----|------------|-------|--------|--------|
| **Quote** (bid/ask update) | 5 | 19 B | 8.2 B | **2.32:1** | 56 ns | 45 ns |
| **Trade** (execution report) | 8 | 35 B | 17.7 B | **1.97:1** | 99 ns | 74 ns |
| **Order** (new order single) | 10 | 34 B | 14.7 B | **2.31:1** | 105 ns | 82 ns |
| **Book Update** (L2 depth) | 7 | 21 B | 10.1 B | **2.08:1** | 73 ns | 56 ns |

Zero roundtrip errors across all message types. Language SDKs (Rust, Python) add FFI call overhead (~40-70ns) on top of the C core.

## When to Use residc

residc fills the gap between zero-copy serializers (SBE, rkyv, Cap'n Proto) that don't compress and block compressors (LZ4, zstd) that can't work per-message:

| Codec | Ratio | Encode | Decode | Per-msg |
|-------|-------|--------|--------|---------|
| **residc** | **2-2.3:1** | **56 ns** | **45 ns** | Yes |
| SBE / rkyv / Cap'n Proto | 1.0:1 | ~10-25 ns | ~0 ns | Yes |
| Protobuf | ~1.3:1 | ~100 ns | ~80 ns | Yes |
| LZ4 (per-msg) | ~1.0:1 | ~50 ns | ~30 ns | Yes |
| zstd (64KB block) | ~2.5:1 | — | — | No |

**Use residc when bandwidth matters**: WAN distribution, cloud feeds, multicast to N consumers, cross-datacenter replication, mobile delivery. On links below ~1 GbE, the 2-3x smaller messages more than pay for the encode/decode cost. On 10 GbE same-rack, use SBE — bandwidth is free.

## Implementations

The C core is the single canonical implementation. Language SDKs wrap it via FFI. See [Wire Format Specification](doc/WIRE_FORMAT.md).

| | C core | Rust SDK | Python SDK |
|--|--------|----------|------------|
| Path | `core/` | `rust/` | `sdk/python/` |
| Mechanism | Direct | FFI via `cc` crate | FFI via ctypes |
| Dependencies | 0 | cc (build only) | 0 |

## Quick Start (C)

```c
#include "residc.h"

typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} Quote;

static const residc_field_t fields[] = {
    { RESIDC_TIMESTAMP,  offsetof(Quote, timestamp),     8, -1 },
    { RESIDC_INSTRUMENT, offsetof(Quote, instrument_id), 2, -1 },
    { RESIDC_PRICE,      offsetof(Quote, price),         4, -1 },
    { RESIDC_QUANTITY,   offsetof(Quote, quantity),       4, -1 },
    { RESIDC_BOOL,       offsetof(Quote, side),           1, -1 },
};

static const residc_schema_t schema = {
    .fields = fields, .num_fields = 5, .msg_size = sizeof(Quote),
};

residc_state_t enc, dec;
residc_init(&enc, &schema);
residc_init(&dec, &schema);

Quote q = { .timestamp = 34200000000000, .instrument_id = 42,
            .price = 1500250, .quantity = 100, .side = 0 };

uint8_t buf[64];
int len = residc_encode(&enc, &q, buf, sizeof(buf));

Quote decoded;
residc_decode(&dec, buf, len, &decoded);
// decoded == q (bit-perfect)
```

## Quick Start (Rust)

```rust
use residc::{Codec, FieldType};

let fields = &[
    FieldType::Timestamp,
    FieldType::Instrument,
    FieldType::Price,
    FieldType::Quantity,
    FieldType::Enum,
];

let mut enc = Codec::new(fields, None).unwrap();
let mut dec = Codec::new(fields, None).unwrap();

let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
let compressed = enc.encode(&values).unwrap();
let decoded = dec.decode(&compressed).unwrap();
assert_eq!(&values[..], &decoded[..]);
```

## Field Types

| Type | Prediction Strategy | Use for |
|------|-------------------|---------|
| `Timestamp` | EMA of inter-message gaps + adaptive k | Nanosecond timestamps |
| `Instrument` | MFU table (top 256 by frequency) | Security/instrument IDs |
| `Price` | Per-instrument last price + penny normalization | Fixed-point prices |
| `Quantity` | Per-instrument last qty + zero-residual flag + round-lot | Share/lot quantities |
| `SequentialId` | Delta from last (per-instrument or global) + adaptive k | Order IDs, execution IDs |
| `Enum` | Same-as-last flag | Side (B/S), order type, TIF |
| `Bool` | None (1 bit) | Flags |
| `Categorical` | Same-as-last flag | Account IDs, firm codes |
| `DeltaPrice` | Delta from a reference price field in same message | Ask price (delta from bid) |
| `DeltaId` | Delta from a reference ID field in same message | Original order ref |
| `Computed` | Not transmitted (0 bits on wire) | Derived fields (leaves_qty) |
| `Raw` | None (verbatim) | Unpredictable fields |

## How It Works

For each message:

1. **Predict** each field from synchronized encoder/decoder state
2. **Compute residual** = actual - predicted
3. **Zigzag encode** the signed residual to unsigned: `0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ...`
4. **Tiered encode** the unsigned residual (parameterized by k):

```
Tier 0:  0    + k bits           values < 2^k            cost: 1+k bits
Tier 1:  10   + (k+6) bits      values < 2^(k+6)        cost: 2+k+6 bits
Tier 2:  110  + (k+12) bits     values < 2^(k+12)       cost: 3+k+12 bits
Tier 3:  1110 + (k+20) bits     values < 2^(k+20)       cost: 4+k+20 bits
Tier 4:  1111 + 64 bits         any value                cost: 68 bits
```

5. **Update state** identically on both encoder and decoder

The k parameter adapts to market regime (CALM vs VOLATILE), detected every 64 messages from average price residual magnitude.

Each compressed message is independently framed: `[1-byte length][payload]`. If compressed size >= raw size, a literal fallback frame (`0xFF` marker) is used, guaranteeing worst-case expansion of 1 byte.

## Cross-Machine Operation

Encoder and decoder run on different machines. They stay synchronized through identical state evolution -- both compute the same predictions from the same message stream. No shared memory, no coordination protocol. The wire carries only compressed frames.

## Gap Recovery (State Checkpoint)

If messages are lost in transit, encoder and decoder state diverges. To recover:

1. **Snapshot** decoder state periodically (e.g., every N messages)
2. **Detect** the gap (sequence number jump, transport-layer notification)
3. **Restore** the snapshot and **replay** from that point

```c
// C
residc_state_t checkpoint;
residc_snapshot(&decoder, &checkpoint);  // periodically

// ... gap detected ...
residc_restore(&decoder, &checkpoint);   // restore
// replay messages from checkpoint onwards
```

```rust
// Rust SDK
let checkpoint = decoder.snapshot().unwrap();  // periodically

// ... gap detected ...
decoder.restore(&checkpoint);                  // restore
// replay messages from checkpoint onwards
```

The snapshot is a full copy of the codec state (~330KB). For most applications, snapshotting every 1000-10000 messages balances recovery speed vs memory overhead.

## MFU Pre-Seeding

By default, the MFU (Most-Frequently-Used) instrument table starts empty and learns from the message stream. If you know the instrument distribution ahead of time, pre-seed it for better compression from message 1:

```c
// C — seed with top instruments by frequency
uint16_t ids[]    = { 42, 99, 7, 101, 55 };
uint16_t counts[] = { 500, 300, 200, 150, 100 };
residc_mfu_seed(&encoder.mfu, ids, counts, 5);
residc_mfu_seed(&decoder.mfu, ids, counts, 5);  // must match
```

```rust
// Rust SDK
let ids = [42u16, 99, 7, 101, 55];
let counts = [500u16, 300, 200, 150, 100];
encoder.seed_mfu(&ids, &counts);
decoder.seed_mfu(&ids, &counts);  // must match
```

## SDK (C-based, cross-language)

The SDK wraps the C core into an opaque-handle API suitable for FFI bindings:

```bash
cd sdk
make              # builds libresdc.so and libresdc.a
```

### Python

```python
from residc import Codec, TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL

enc = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])
dec = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])

data = enc.encode([34200000000000, 42, 1500250, 100, 0])
values = dec.decode(data)
assert values == [34200000000000, 42, 1500250, 100, 0]
```

The Python SDK uses ctypes with zero dependencies. See `sdk/python/example.py` for a complete example.

## Building

### C

```bash
# Two files, no dependencies
cc -O2 -o quote_example examples/custom/quote_example.c core/residc.c -Icore
./quote_example
```

### Rust SDK

```bash
cd rust
cargo test     # 7 tests + doctest
cargo bench    # criterion benchmarks
```

## Technical Paper

See [doc/TECHNIQUE.md](doc/TECHNIQUE.md) for the full technical description: prediction strategies, tiered residual coding, adaptive k, regime detection, framing, and state synchronization.

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or http://www.apache.org/licenses/LICENSE-2.0)
- MIT License ([LICENSE-MIT](LICENSE-MIT) or http://opensource.org/licenses/MIT)

at your option.
