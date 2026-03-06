# residc (Rust)

`#![no_std]`, zero-dependency, zero-allocation prediction-residual codec for financial data.

Same wire format as the C library. A message encoded by C can be decoded by Rust and vice versa. Encoder and decoder can run on different machines — they stay synchronized through identical state evolution, no shared memory needed.

## Performance

Measured with [Criterion](https://github.com/bheisler/criterion.rs) on synthetic quote data (5 fields: timestamp, instrument, price, quantity, side), 10,000 messages per iteration, `--release` with LTO.

| Metric | Value |
|--------|-------|
| Encode latency | **96 ns/msg** |
| Decode latency | **55 ns/msg** |
| Compression ratio | 2.3:1 (19 bytes raw -> ~8 bytes compressed) |
| Encode throughput | 198 MB/s |
| Decode throughput | 345 MB/s |
| `no_std` | Yes |
| Dependencies | 0 |
| Allocations per encode/decode | 0 |
| Heap usage | 0 (fully stack-allocated, ~330KB per Codec) |

### vs SBE total delivery time

SBE encodes in ~25ns (pointer cast, zero compression) but sends full-size messages. residc compresses ~3x, trading encode cycles for wire time.

| Link | SBE (encode + wire) | residc (encode + wire + decode) | Winner |
|------|--------------------|---------------------------------|--------|
| 10 GbE, 19B msg | 25ns + 15ns = **40ns** | 96ns + 5ns + 55ns = **156ns** | SBE |
| 1 GbE, 60B msg | 25ns + 480ns = **505ns** | 96ns + 160ns + 55ns = **311ns** | residc |
| 100 Mbps WAN | 25ns + 4.8us = **4.8us** | 96ns + 1.6us + 55ns = **1.8us** | residc |
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
cargo bench
```

## Running tests

```bash
cargo test
```

14 tests: bit I/O (5), MFU table (3), zigzag + tiered residual coding (3), full encode/decode roundtrip (1K messages), compression ratio (10K messages), doctest.
