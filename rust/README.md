# residc (Rust)

Zero-dependency Rust implementation of the residc prediction-residual codec for financial data.

Same wire format as the C library. A message encoded by C can be decoded by Rust and vice versa.

## Performance

Measured with [Criterion](https://github.com/bheisler/criterion.rs) on synthetic quote data (5 fields: timestamp, instrument, price, quantity, side), 10,000 messages per iteration, `--release` with LTO.

| Metric | Value |
|--------|-------|
| Encode latency | 105 ns/msg |
| Decode latency | 54 ns/msg |
| Compression ratio | 2.3:1 (19 bytes raw -> ~8 bytes compressed) |
| Encode throughput | 181 MB/s |
| Decode throughput | 352 MB/s |
| Dependencies | 0 |
| Unsafe | BitWriter/BitReader only (unchecked buffer access) |
| Allocations per encode/decode | 0 |

### vs SBE total delivery time

SBE encodes in ~25ns but sends uncompressed. residc compresses 3x, reducing wire time.

| Link | SBE (encode + wire) | residc (encode + wire + decode) |
|------|--------------------|---------------------------------|
| 1 GbE | 25ns + 152ns = **177ns** | 105ns + 51ns + 54ns = **210ns** |
| 1 GbE (full msg) | 25ns + 480ns = **505ns** | 105ns + 160ns + 54ns = **319ns** |
| 100 Mbps | 25ns + 4.8us = **4.8us** | 105ns + 1.6us + 54ns = **1.8us** |

On bandwidth-constrained links, residc wins on total delivery time despite slower encoding.

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
cargo bench
```

## Running tests

```bash
cargo test
```

14 tests cover bit I/O, MFU table, zigzag + tiered residual coding, full encode/decode roundtrip (1K messages), and compression ratio verification (10K messages).
