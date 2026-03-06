# residc (Rust)

Rust SDK for the residc prediction-residual compression codec. Wraps the C core via FFI with a safe, idiomatic Rust API.

## Usage

```rust
use residc::{Codec, FieldType};

let fields = &[
    FieldType::Timestamp,
    FieldType::Instrument,
    FieldType::Price,
    FieldType::Quantity,
    FieldType::Enum,
];

let mut encoder = Codec::new(fields, None).unwrap();
let mut decoder = Codec::new(fields, None).unwrap();

let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
let compressed = encoder.encode(&values).unwrap();
let decoded = decoder.decode(&compressed).unwrap();

assert_eq!(&values[..], &decoded[..]);
```

## Performance

Measured on 5-field synthetic quotes, `--release` with LTO + `target-cpu=native`.

| Metric | Value |
|--------|-------|
| Encode | ~122 ns/msg |
| Decode | ~91 ns/msg |

FFI overhead (~40-70ns) accounts for the difference vs the raw C benchmark (51ns/39ns). For latency-critical paths, use the C API directly.

## Building

The `cc` crate compiles `core/residc.c` and `sdk/residc_sdk.c` as part of `cargo build` — no separate build step needed.

```bash
cargo test     # 7 tests + doctest
cargo bench    # criterion benchmarks
```

## API

- `Codec::new(fields, ref_fields)` — Create encoder or decoder
- `codec.encode(values)` — Compress field values to bytes
- `codec.decode(data)` — Decompress bytes to field values
- `codec.snapshot()` / `codec.restore(snap)` — Gap recovery
- `codec.reset()` — Reset to initial state
- `codec.seed_mfu(ids, counts)` — Pre-seed instrument frequency table
