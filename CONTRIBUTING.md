# Contributing to residc

Thanks for your interest in contributing! This guide covers building, testing, and submitting changes.

## Building

### C

```bash
cc -O2 -o test examples/custom/quote_example.c core/residc.c -Icore
./test
```

### Rust

```bash
cd rust
cargo test
```

### SDK

```bash
cd sdk
make        # builds libresdc.so and libresdc.a
```

## Running Tests

### C

```bash
make test     # runs all C unit tests in test/
```

Or manually:

```bash
cc -O2 -o quote_example examples/custom/quote_example.c core/residc.c -Icore
./quote_example
```

### Rust

```bash
cd rust
cargo test    # 7 tests + 1 doctest
```

### Python SDK

```bash
cd sdk && make          # build the shared library first
cd python && python3 example.py
```

## Benchmarks

### C

```bash
cd bench
cc -O2 -march=native -o bench_quote bench_quote.c ../core/residc.c -I../core
./bench_quote
```

### Rust

```bash
cd rust
RUSTFLAGS="-C target-cpu=native" cargo bench
```

## Coding Style

- **C**: K&R style, 4-space indent
- **Rust**: Standard `rustfmt` formatting

## Submitting Changes

1. Fork the repository
2. Create a feature branch (`git checkout -b my-feature`)
3. Make your changes
4. Run all tests (C examples, `cargo test`, Python example)
5. Submit a pull request against `master`

Please keep PRs focused on a single change. If you're fixing a bug and adding a feature, submit them as separate PRs.

## Reporting Issues

Use the GitHub issue templates for bug reports and feature requests.

## License

By contributing, you agree that your contributions will be licensed under the same dual license as the project (MIT OR Apache-2.0).
