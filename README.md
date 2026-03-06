# residc

**Schema-driven, per-message prediction-residual compression for financial data.**

residc compresses financial messages (quotes, orders, trades) **3-4x** by predicting each field from context and encoding only the prediction error. Unlike general-purpose compressors (LZ4, zstd), it works on individual messages -- no blocks, no buffering, per-message random access.

Structure-agnostic: you define any message struct, map fields to prediction types, and the codec handles the rest.

## Benchmarks

### Compression

```
                Raw    Compressed   Ratio    Errors
ITCH 5.0      32.2 B    9.9 B     3.27:1      0     (8M real NASDAQ messages)
Quotes        27.0 B   11.6 B     2.34:1      0     (100K synthetic quotes)
Orders        43.7 B   13.4 B     3.26:1      0     (2M synthetic order flow)
```

### Latency

|  | C (gcc -O2, 100K quotes) | Rust (--release, LTO, 100K quotes) |
|--|--------------------------|-------------------------------------|
| Encode | **51 ns/msg** | **52 ns/msg** |
| Decode | 46 ns/msg | **39 ns/msg** |

Both implementations use `always_inline` / `#[inline(always)]` on the entire hot path. Rust achieves parity on encode and is faster on decode through hash-accelerated MFU lookup, direct-write BitWriter (no intermediate buffer), and single-pass encode+commit.

## Comparison to Alternatives

### Per-message codecs (apples to apples)

| Codec | Ratio (ITCH) | Encode | Decode | Per-msg | Approach |
|-------|-------------|--------|--------|---------|----------|
| **residc** | **3.27:1** | **51 ns** | **39 ns** | Yes | Prediction-residual |
| SBE | 1.00:1 | ~25 ns | ~0 ns | Yes | Zero-copy cast, no compression |
| FAST (FIX) | 2-4:1 | ~200 ns | ~200 ns | Yes | Template delta + stop-bit coding |
| Protobuf | ~1.3:1 | ~150 ns | ~120 ns | Yes | Varint, no cross-message state |
| Cap'n Proto | 1.00:1 | ~10 ns | ~0 ns | Yes | Zero-copy, no compression |
| FlatBuffers | 1.00:1 | ~15 ns | ~0 ns | Yes | Zero-copy, no compression |
| LZ4 per-msg | 1.02:1 | ~50 ns | ~30 ns | Yes | Byte-sequence matching (too small) |

### Block codecs (different trade-off: no random access)

| Codec | Ratio (ITCH) | Per-msg | Notes |
|-------|-------------|---------|-------|
| LZ4 block (64KB) | 1.80:1 | No | Needs buffering |
| zstd block (64KB) | ~2.5:1 | No | Needs buffering |
| zstd (dict) | ~2.8:1 | No | Requires pre-trained dictionary |

### When to use what

| Scenario | Best choice | Why |
|----------|------------|-----|
| Same-rack ultra-low-latency (FPGA, kernel bypass) | SBE / Cap'n Proto | 25ns encode, bandwidth is free |
| WAN market data distribution | **residc** | 3x smaller = 3x more throughput, wire time dominates |
| Cloud / multi-region feeds | **residc** | Bandwidth costs money, latency budget is microseconds |
| Multicast to N consumers | **residc** | Compression paid once, wire savings multiplied N times |
| Historical data storage | **residc** + block compressor | Per-message access + block-level ratio |
| Cross-datacenter replication | **residc** | Every byte costs on leased lines |
| Mobile / retail data delivery | **residc** | 30:1 on text vs raw, enables cheap data products |

### Total delivery time: residc vs SBE

SBE is faster to encode but sends larger messages. The real metric is **end-to-end**: encode + wire + decode.

![Total Delivery Time: SBE vs residc](doc/img/delivery_time.svg)

| Link | SBE total | residc total | Winner |
|------|----------|-------------|--------|
| 10 GbE, 19B msg | 25ns + 15ns = **40ns** | 52ns + 5ns + 39ns = **96ns** | SBE |
| 1 GbE, 19B msg | 25ns + 152ns = **177ns** | 52ns + 51ns + 39ns = **142ns** | **residc** |
| 1 GbE, 60B msg | 25ns + 480ns = **505ns** | 52ns + 160ns + 39ns = **251ns** | **residc** |
| 100 Mbps WAN | 25ns + 4.8us = **4.8us** | 52ns + 1.6us + 39ns = **1.7us** | **residc** |
| Multicast x10, 1GbE | 25ns + 10*480ns = **4.8us** | 52ns + 10*160ns + 39ns = **1.7us** | **residc** |

**The crossover point is ~1 GbE.** Above that, SBE wins on encode speed. Below that, residc wins because 3x smaller messages travel 3x faster on the wire. For data distribution — where vendors serve thousands of consumers over WAN, cloud, or multicast — the compression pays for itself many times over.

### vs FAST Protocol

FAST (FIX Adapted for STreaming) was the FIX Trading Community's answer to this exact problem. It used template-based delta encoding with stop-bit coding. residc differs in:

- **Prediction quality**: FAST uses simple delta. residc uses EMA (timestamps), MFU tables (instruments), per-instrument tracking (prices), regime detection (adaptive k). Better predictions = smaller residuals.
- **Coding efficiency**: FAST uses stop-bit coding (7 useful bits per byte). residc uses tiered variable-width codes at bit granularity. More compact for small values.
- **Simplicity**: residc is two C files (1500 lines) or one Rust crate (1260 lines). FAST implementations are typically 10-50K lines.
- **Status**: FAST is being deprecated. CME discontinued FAST feeds in 2023. SBE replaced it for low-latency; residc fills the compression niche that FAST left behind.

## Implementations

| | C | Rust |
|--|---|------|
| Files | `core/residc.h` + `core/residc.c` | `rust/src/` (4 modules) |
| Lines | 1,498 | 1,261 |
| Dependencies | 0 | 0 |
| `no_std` | N/A | Yes |
| Heap allocations | 0 | 0 |
| Encode latency | 51 ns | **52 ns** |
| Decode latency | 46 ns | **39 ns** |

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
use residc::{Schema, FieldType, Codec, Message};

let schema = Schema::builder()
    .field("timestamp", FieldType::Timestamp)
    .field("instrument", FieldType::Instrument)
    .field("price", FieldType::Price)
    .field("quantity", FieldType::Quantity)
    .field("side", FieldType::Bool)
    .build();

let mut enc = Codec::new(&schema);
let mut dec = Codec::new(&schema);

let msg = Message::new()
    .set(0, 34_200_000_000_000u64)
    .set(1, 42u64)
    .set(2, 1_500_250u64)
    .set(3, 100u64)
    .set(4, 0u64);

let mut buf = [0u8; 64];
let len = enc.encode(&msg, &mut buf).unwrap();
let decoded = dec.decode(&buf[..len]).unwrap();
assert_eq!(decoded.get(2), 1_500_250);
```

## Field Types

| Type | Prediction Strategy | Use for |
|------|-------------------|---------|
| `Timestamp` | EMA of inter-message gaps + adaptive k | Nanosecond timestamps |
| `Instrument` | MFU table (top 64 by frequency) | Security/instrument IDs |
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
Tier 0:  0                       value = 0               cost: 1 bit
Tier 1:  10  + k bits            values [1, 2^k]         cost: 2+k bits
Tier 2:  110 + 2k bits           next 2^(2k) values      cost: 3+2k bits
Tier 3:  1110 + 3k bits          next 2^(3k) values      cost: 4+3k bits
Tier 4:  11110 + 32 bits         any 32-bit value         cost: 37 bits
Tier 5:  11111 + 64 bits         any 64-bit value         cost: 69 bits
```

5. **Update state** identically on both encoder and decoder

The k parameter adapts to market regime (CALM vs VOLATILE), detected every 64 messages from average price residual magnitude.

Each compressed message is independently framed: `[1-byte length][payload]`. If compressed size >= raw size, a literal fallback frame (`0xFF` marker) is used, guaranteeing worst-case expansion of 1 byte.

## Cross-Machine Operation

Encoder and decoder run on different machines. They stay synchronized through identical state evolution -- both compute the same predictions from the same message stream. No shared memory, no coordination protocol. The wire carries only compressed frames.

## Building

### C

```bash
# Two files, no dependencies
cc -O2 -o quote_example examples/custom/quote_example.c core/residc.c -Icore
./quote_example
```

### Rust

```bash
cd rust
cargo test     # 14 tests
cargo bench    # criterion benchmarks
```

## Technical Paper

See [doc/TECHNIQUE.md](doc/TECHNIQUE.md) for the full technical description: prediction strategies, tiered residual coding, adaptive k, regime detection, framing, and state synchronization.

## License

MIT
