# residc (Rust)

`#![no_std]`, zero-dependency, zero-allocation prediction-residual codec for financial data.

**Note:** The Rust and C implementations use different wire formats and are not interoperable. Each implementation is self-consistent (encoder ↔ decoder within the same implementation works perfectly). For cross-language interop, use the [C SDK](../sdk/) which provides bindings for Python and other languages.

Encoder and decoder can run on different machines — they stay synchronized through identical state evolution, no shared memory needed.

## Performance

Measured on synthetic quote data (5 fields: timestamp, instrument, price, quantity, side), `--release` with LTO + `target-cpu=native`.

| Metric | Value | Method |
|--------|-------|--------|
| Encode latency | **49 ns/msg** | criterion mean (100 samples) |
| Decode latency | **42 ns/msg** | criterion mean (100 samples) |
| Compression ratio | 2.37:1 (19 bytes raw -> ~8 bytes compressed) |
| Encode throughput | 365 MB/s |
| Decode throughput | 487 MB/s |
| `no_std` | Yes |
| Dependencies | 0 |
| Allocations per encode/decode | 0 |
| Heap usage | 0 (fully stack-allocated, ~330KB per Codec) |

### vs C implementation

| | C (gcc -O2, best of 10) | Rust (criterion mean) |
|--|------------------------|----------------------|
| Encode | 51 ns/msg | **49 ns/msg** |
| Decode | 48 ns/msg | **42 ns/msg** |

Comparable performance. Rust uses hash-accelerated MFU lookup, direct-write BitWriter, and single-pass encode+commit.

### vs SBE total delivery time

SBE encodes in ~25ns (pointer cast, zero compression) but sends full-size messages. residc compresses ~3x, trading encode cycles for wire time.

| Link | SBE (encode + wire) | residc (encode + wire + decode) | Winner |
|------|--------------------|---------------------------------|--------|
| 10 GbE, 19B msg | 25ns + 15ns = **40ns** | 49ns + 5ns + 42ns = **96ns** | SBE |
| 1 GbE, 60B msg | 25ns + 480ns = **505ns** | 49ns + 160ns + 42ns = **251ns** | residc |
| 100 Mbps WAN | 25ns + 4.8us = **4.8us** | 49ns + 1.6us + 42ns = **1.7us** | residc |
| Multicast (N consumers) | N * 480ns wire | N * 160ns wire | residc |

SBE wins in same-rack ultra-low-latency setups where bandwidth is free. residc wins everywhere bandwidth costs — WAN, cloud, data distribution, multicast, congested links.

## Usage

```rust
use residc::{Schema, FieldType, Codec, Message};

let schema = Schema::builder()
    .field("timestamp", FieldType::Timestamp)
    .field("instrument", FieldType::Instrument)
    .field("price", FieldType::Price)
    .field("quantity", FieldType::Quantity)
    .field("side", FieldType::Bool)
    .build();

let mut encoder = Codec::new(&schema);
let mut decoder = Codec::new(&schema);

let msg = Message::new()
    .set(0, 34_200_000_000_000u64)
    .set(1, 42)
    .set(2, 1_500_250)
    .set(3, 100)
    .set(4, 0);

let mut buf = [0u8; 64];
let len = encoder.encode(&msg, &mut buf).unwrap();
let decoded = decoder.decode(&buf[..len]).unwrap();
assert_eq!(decoded.get(2), 1_500_250);
```

## Running benchmarks

```bash
RUSTFLAGS="-C target-cpu=native" cargo bench
# or raw timing (matches C benchmark methodology):
RUSTFLAGS="-C target-cpu=native" cargo run --release --example bench_raw
```

## Running tests

```bash
cargo test
```

16 tests: bit I/O (5), MFU table (4), zigzag + tiered residual coding (3), full encode/decode roundtrip (1K messages), compression ratio (10K messages), snapshot/restore, MFU seed, doctest.
