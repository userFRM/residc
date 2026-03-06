# residc

**Schema-driven, per-message compression for financial data.**

residc compresses financial messages (quotes, orders, trades) **3-4x** by predicting each field from context and encoding only the prediction error. Unlike general-purpose compressors (LZ4, zstd), it works on individual messages — no blocks, no buffering, per-message random access.

```
              Raw    Compressed   Ratio    Errors
ITCH 5.0    32.2 B    9.9 B     3.27:1      0     (8M real NASDAQ messages)
Quotes      27.0 B   11.6 B     2.34:1      0     (100K synthetic quotes)
Orders      43.7 B   13.4 B     3.26:1      0     (2M synthetic order flow)
LZ4/msg     32.2 B   31.5 B     1.02:1      —     (same ITCH data)
LZ4/block   32.2 B   17.9 B     1.80:1      —     (same ITCH data, no random access)
```

## Why

General-purpose compressors fail on small messages. LZ4 gets **1.02:1** on 32-byte ITCH messages — essentially nothing. They need kilobytes of context to find patterns.

Financial data has structure that generic compressors can't exploit:
- Timestamps advance by predictable gaps
- A few instruments account for most messages
- Prices move in small, penny-aligned increments
- Quantities repeat (round lots)
- Order IDs increment sequentially

residc exploits all of these patterns with per-field prediction strategies, then encodes only the residual (prediction error) using tiered variable-width codes.

## Quick Start

```c
#include "residc.h"

// 1. Define your message
typedef struct {
    uint64_t timestamp;
    uint16_t instrument_id;
    uint32_t price;
    uint32_t quantity;
    uint8_t  side;
} Quote;

// 2. Define a schema — map fields to prediction strategies
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

// 3. Compress
residc_state_t enc, dec;
residc_init(&enc, &schema);
residc_init(&dec, &schema);

Quote q = { .timestamp = 34200000000000, .instrument_id = 42,
            .price = 1500250, .quantity = 100, .side = 0 };

uint8_t buf[64];
int len = residc_encode(&enc, &q, buf, sizeof(buf));  // len ≈ 8-15 bytes

Quote decoded;
residc_decode(&dec, buf, len, &decoded);
// decoded == q (bit-perfect)
```

That's it. Define a struct, map fields to types, encode/decode. The library handles prediction, residual coding, and state management.

## Field Types

| Type | Prediction | Use for |
|------|-----------|---------|
| `RESIDC_TIMESTAMP` | EMA of inter-message gaps | Nanosecond timestamps |
| `RESIDC_INSTRUMENT` | MFU table (top 64 by frequency) | Security/instrument IDs |
| `RESIDC_PRICE` | Per-instrument last price + penny normalization | Prices (fixed-point) |
| `RESIDC_QUANTITY` | Per-instrument last quantity + zero-residual flag + round-lot normalization | Share quantities |
| `RESIDC_SEQUENTIAL_ID` | Delta from last (global or per-instrument) | Order IDs, execution IDs |
| `RESIDC_ENUM` | Same-as-last flag | Side (B/S), order type, TIF |
| `RESIDC_BOOL` | None | 1-bit fields |
| `RESIDC_CATEGORICAL` | Same-as-last flag | Account IDs, firm codes |
| `RESIDC_DELTA_ID` | Delta from a reference field in same message | orig_cl_ord_id |
| `RESIDC_DELTA_PRICE` | Delta from a reference price field | ask (delta from bid), last_px |
| `RESIDC_COMPUTED` | Not transmitted (0 bits) | Derived fields (leaves_qty) |
| `RESIDC_RAW` | None (verbatim) | Unpredictable fields |

## How It Works

For each message:

1. **Predict** each field from encoder/decoder state (both maintain identical state)
2. **Compute residual** = actual - predicted
3. **Encode residual** with tiered variable-width code:
   - Tier 0: `0` + k bits (small residuals)
   - Tier 1: `10` + (k+6) bits
   - Tier 2: `110` + (k+12) bits
   - Tier 3: `1110` + (k+20) bits
   - Tier 4: `1111` + 64 raw bits (fallback)
4. **Update state** on both encoder and decoder

The `k` parameter adapts based on market regime (calm vs volatile), so the codec automatically adjusts to changing conditions.

Each compressed message is independently framed: `[1-byte length] [payload]`. No block boundaries. Any message can be decoded given the codec state up to that point.

## Performance

Measured on NASDAQ ITCH 5.0 data (8M real messages, Intel Xeon):

| Metric | Value |
|--------|-------|
| Compression ratio | 3.27:1 |
| Encode throughput | 331 MB/s |
| Decode throughput | 348 MB/s |
| Encode latency | ~100 ns/msg |
| Decode latency | ~95 ns/msg |
| Heap allocations | 0 |
| Roundtrip errors | 0 |

## Building

residc is two files: `core/residc.h` and `core/residc.c`. Add them to your project.

```bash
# Build and run the example
cd examples/custom
cc -O2 -o quote_example quote_example.c ../../core/residc.c -I../../core
./quote_example
```

## Advanced: Building Blocks

If the schema-driven API doesn't fit your needs, you can use the building blocks directly:

```c
// Bit writer
residc_bitwriter_t bw;
residc_bw_init(&bw);
residc_bw_write(&bw, 42, 7);           // write 7 bits
residc_encode_residual(&bw, -13, 3);   // encode signed residual with k=3
int len = residc_bw_finish(&bw);       // flush to bw.buf[]

// Bit reader
residc_bitreader_t br;
residc_br_init(&br, bw.buf, len);
uint64_t val = residc_br_read(&br, 7);          // read 7 bits
int64_t res = residc_decode_residual(&br, 3);    // decode residual

// MFU table
residc_mfu_table_t mfu;
residc_mfu_init(&mfu);
residc_mfu_update(&mfu, instrument_id);
int idx = residc_mfu_lookup(&mfu, instrument_id);  // -1 or 0-63

// Adaptive k
uint64_t sum = 0; uint32_t count = 0;
residc_adaptive_update(&sum, &count, zigzag_value);
int k = residc_adaptive_k(sum, count, 3, 15);
```

## Technique

residc applies prediction-residual coding — a technique from signal processing (JPEG-LS, FLAC, DPCM) — to financial message fields. The key insight is that financial data is a **structured time series** with strong per-field correlations:

- Timestamps are quasi-periodic → EMA prediction captures the rhythm
- Instrument distribution follows Zipf's law → MFU table captures top 64
- Prices are mean-reverting per instrument → per-instrument delta is small
- Quantities cluster at round lots → zero-residual flag + normalization
- IDs are sequential → delta is near-zero

By combining domain-specific predictions with adaptive tiered coding, residc achieves 3-4x compression on individual messages where general-purpose compressors achieve ~1x.

See [TECHNIQUE.md](doc/TECHNIQUE.md) for the full technical description.

## License

MIT
