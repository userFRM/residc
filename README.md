# residc

[![CI](https://github.com/userFRM/residc/actions/workflows/ci.yml/badge.svg)](https://github.com/userFRM/residc/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT%2FApache--2.0-blue)](LICENSE-MIT)

**Per-message prediction-residual compression for financial market data.**

residc compresses individual financial messages (quotes, trades, orders) **~2x** by predicting each field from context and encoding only the residual. No blocks, no buffering — each message compresses and decompresses independently.

## Features

- **Per-message**: every message is independently accessible, no block boundaries
- **Schema-driven**: define your struct, map fields to prediction types, done
- **Sub-100ns**: encode + decode in under 100ns per message on the C core
- **Zero-copy decode**: no heap allocation, no intermediate buffers
- **Deterministic**: bit-identical output on all conforming implementations
- **Cross-language**: single C core with Rust and Python SDKs via FFI

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [Benchmarks](#benchmarks)
- [Field Types](#field-types)
- [How It Works](#how-it-works)
- [Documentation](#documentation)
- [Building from Source](#building-from-source)
- [Contributing](#contributing)
- [License](#license)

## Installation

### Rust

```toml
# Cargo.toml
[dependencies]
residc = "0.2"
```

### Python

```bash
# Copy the shared library to the package directory
cd sdk && make
cp libresdc.so python/residc/
```

### C

No package manager needed. Copy `core/residc.h` and `core/residc.c` into your project.

## Quick Start

### C

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
// decoded == q (bit-perfect roundtrip)
```

### Rust

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

### Python

```python
from residc import Codec, TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL

enc = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])
dec = Codec([TIMESTAMP, INSTRUMENT, PRICE, QUANTITY, BOOL])

data = enc.encode([34200000000000, 42, 1500250, 100, 0])
values = dec.decode(data)
assert values == [34200000000000, 42, 1500250, 100, 0]
```

## Benchmarks

100K synthetic messages per type, gcc -O2 `-march=native`, best of 10 iterations. Fully reproducible:

```bash
cc -O2 -march=native -o bench bench/bench_compression.c core/residc.c -Icore && ./bench
```

| Message | Fields | Raw | Compressed | Ratio | Encode | Decode |
|---------|--------|-----|------------|-------|--------|--------|
| **Quote** (bid/ask update) | 5 | 19 B | 8.2 B | **2.32:1** | 56 ns | 45 ns |
| **Trade** (execution report) | 8 | 35 B | 17.7 B | **1.97:1** | 99 ns | 74 ns |
| **Order** (new order single) | 10 | 34 B | 14.7 B | **2.31:1** | 105 ns | 82 ns |
| **Book Update** (L2 depth) | 7 | 21 B | 10.1 B | **2.08:1** | 73 ns | 56 ns |

Zero roundtrip errors across all message types. SDK bindings (Rust, Python) add FFI overhead (~40-70ns).

### Comparison

| Codec | Ratio | Per-msg encode | Per-msg decode | Per-message? |
|-------|-------|----------------|----------------|:------------:|
| **residc** | **2-2.3:1** | **56 ns** | **45 ns** | Yes |
| SBE / Cap'n Proto | 1.0:1 | ~10-25 ns | ~0 ns | Yes |
| Protobuf | ~1.3:1 | ~100 ns | ~80 ns | Yes |
| LZ4 (per-msg) | ~1.0:1 | ~50 ns | ~30 ns | Yes |
| zstd (64KB block) | ~2.5:1 | -- | -- | No |

**Use residc when bandwidth matters**: WAN distribution, cloud feeds, multicast fanout, cross-datacenter replication. On links below ~1 GbE, the ~2x smaller messages more than pay for the encode/decode cost. On 10 GbE same-rack, use SBE.

## Field Types

| Type | Prediction | Use for |
|------|------------|---------|
| `Timestamp` | EMA of inter-message gaps + adaptive k | Nanosecond timestamps |
| `Instrument` | MFU table (top 256 by frequency) | Security/instrument IDs |
| `Price` | Per-instrument last price + penny normalization | Fixed-point prices |
| `Quantity` | Per-instrument last qty + round-lot normalization | Share/lot quantities |
| `SequentialId` | Delta from last + adaptive k | Order IDs, trade IDs |
| `Enum` | Same-as-last flag | Side (B/S), order type, TIF |
| `Bool` | None (1 bit) | Flags |
| `Categorical` | Same-as-last flag | Account IDs, firm codes |
| `DeltaPrice` | Delta from reference price in same message | Ask (delta from bid) |
| `DeltaId` | Delta from reference ID in same message | Original order ref |
| `Computed` | Not transmitted (0 bits) | Derived fields |
| `Raw` | None (verbatim) | Unpredictable fields |

## How It Works

For each message, the codec:

1. **Predicts** each field from synchronized encoder/decoder state
2. **Computes the residual** (actual - predicted)
3. **Zigzag-encodes** the signed residual to unsigned
4. **Tiered-encodes** with an adaptive k parameter (small residuals = fewer bits)
5. **Updates state** identically on both sides

Each compressed message is independently framed: `[1-byte length][payload]`. If compressed >= raw, a literal fallback is used (worst-case expansion: 1 byte).

Encoder and decoder stay synchronized through identical state evolution -- no shared memory, no coordination protocol. See the [technical paper](doc/TECHNIQUE.md) and [wire format spec](doc/WIRE_FORMAT.md) for full details.

## Documentation

| Document | Description |
|----------|-------------|
| [TECHNIQUE.md](doc/TECHNIQUE.md) | Full technical paper: prediction strategies, tiered coding, adaptive k, regime detection |
| [WIRE_FORMAT.md](doc/WIRE_FORMAT.md) | Wire format specification for the C core |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Build instructions, test guide, PR process |

## Building from Source

### C

```bash
# Core: two files, zero dependencies
cc -O2 -o example examples/custom/quote_example.c core/residc.c -Icore

# SDK shared library
cd sdk && make    # produces libresdc.so and libresdc.a
```

### Rust SDK

```bash
cd rust
cargo test      # 7 tests + doctest
cargo bench     # criterion benchmarks
```

### Benchmarks

```bash
cc -O2 -march=native -o bench_quote bench/bench_quote.c core/residc.c -Icore
cc -O2 -march=native -o bench_compression bench/bench_compression.c core/residc.c -Icore
./bench_quote
./bench_compression
```

## Architecture

```
residc/
  core/           C codec (residc.h + residc.c) -- the single source of truth
  sdk/            C SDK: opaque-handle API wrapping the core
    python/       Python bindings (ctypes, zero dependencies)
  rust/           Rust SDK (FFI via cc crate)
  bench/          C benchmarks (latency + compression)
  doc/            Technical paper + wire format spec
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, coding standards, and the PR process.

## License

Dual-licensed under your choice of:

- [MIT](LICENSE-MIT)
- [Apache 2.0](LICENSE-APACHE)
