# Prediction-Residual Compression for Financial Messages

## Abstract

We describe a per-message, streaming compression technique for financial market data that achieves 3.27:1 compression on real NASDAQ ITCH 5.0 data (8 million messages) with zero roundtrip errors. Unlike general-purpose compressors that require block context, our approach exploits the structured, correlated nature of financial messages through per-field prediction and tiered residual coding. Each message is independently decodable given the codec state, enabling random access without block boundaries.

## 1. Problem Statement

Financial messaging protocols generate massive volumes of small, structured messages. A typical NASDAQ ITCH Add Order message is 36 bytes; a quote update might be 20-40 bytes. At millions of messages per second, bandwidth becomes significant — but each individual message is too small for general-purpose compressors to exploit.

**Why general-purpose compressors fail on small messages:**

| Compressor | ITCH ratio | Notes |
|-----------|-----------|-------|
| LZ4 per-message | 1.02:1 | No dictionary context, message too small |
| zstd per-message | ~1.1:1 | Slightly better, still minimal |
| LZ4 block (64KB) | 1.80:1 | Needs buffering, loses random access |
| zstd block (64KB) | ~2.5:1 | Same trade-off |
| **residc** | **3.27:1** | Per-message, streaming |

LZ4 and zstd look for repeated byte sequences. A 36-byte message doesn't have enough bytes for substring matching to work. Our approach instead uses **semantic prediction** — we know what each field means and can predict its value from context.

## 2. Core Technique: Predict, Residual, Code

For each field in a message:

```
residual = actual_value - predicted_value
compressed = tiered_encode(zigzag(residual), k)
```

The encoder and decoder maintain **identical synchronized state**. Both compute the same prediction for each field, so only the residual needs to be transmitted. If predictions are accurate, residuals are small, and small values compress well with variable-width codes.

### 2.1 Prediction Strategies

Each field type has a domain-specific prediction strategy:

**Timestamps (EMA gap prediction):**
```
predicted_gap = exponential_moving_average(recent gaps)
residual = actual_gap - predicted_gap
```
Financial timestamps are quasi-periodic within a trading session. The EMA adapts to changing message rates (e.g., market open vs midday).

**Instruments (Most-Frequently-Used table):**
```
if instrument == last_instrument:  emit 0 (1 bit)
elif instrument in MFU_table:      emit 1,0,index (1+1+6 = 8 bits)
else:                              emit 1,1,raw_id (1+1+14 = 16 bits)
```
Trading volume follows Zipf's law: a handful of instruments generate the majority of messages. A 64-entry MFU table with hash+chain lookup captures the top instruments with a 6-bit index (vs 14 bits raw).

**Prices (per-instrument delta with penny normalization):**
```
predicted = last_price_for_this_instrument
residual = actual_price - predicted
if both_on_penny_boundary: residual /= 100  (penny normalization)
```
Most price changes are small (a few ticks). Penny normalization further reduces residuals for instruments that trade on penny increments.

**Quantities (per-instrument with zero-residual flag):**
```
predicted = last_quantity_for_this_instrument
if actual == predicted: emit 0 (1 bit, ~45% of the time)
else: emit 1, encode_residual(actual - predicted)
```
Quantities frequently repeat (e.g., round lots of 100). The zero-residual flag saves 5-10 bits whenever the prediction is exact.

**Sequential IDs (delta coding):**
```
predicted = last_id_seen (global or per-instrument)
residual = actual_id - predicted  (typically 1-10)
```
Order IDs, execution IDs, and tracking numbers increment sequentially. Delta coding reduces 64-bit IDs to a few bits.

### 2.2 Tiered Residual Coding

All residuals are encoded with the same tiered variable-width code:

```
  signed_residual → zigzag → unsigned → tiered_encode
```

**Zigzag encoding** maps signed integers to unsigned:
```
 0 → 0,  -1 → 1,  1 → 2,  -2 → 3,  2 → 4, ...
```

**Tiered code** (parameterized by k):
```
Tier 0:  0                     value = 0               cost: 1 bit
Tier 1:  10  + k bits          values [1, 2^k]         cost: 2+k bits
Tier 2:  110 + 2k bits         next 2^(2k) values      cost: 3+2k bits
Tier 3:  1110 + 3k bits        next 2^(3k) values      cost: 4+3k bits
Tier 4:  11110 + 32 bits       any 32-bit value         cost: 37 bits
Tier 5:  11111 + 64 bits       any 64-bit value         cost: 69 bits
```

Tier 0 encodes the most common case (prediction is exact, residual = 0) in a single bit. Tiers 1-3 use progressively wider payload fields parameterized by k. Tiers 4-5 are escape tiers for outliers.

The `k` parameter controls the trade-off: smaller k is better when residuals are small (accurate predictions); larger k is better when residuals are large (volatile markets).

### 2.3 Adaptive k (JPEG-LS Style)

The k parameter adapts based on recent residual magnitudes:

```
k = floor(log2(running_average(|zigzag(residuals)|) + 1))
k = clamp(k, min_k, max_k)
```

The running average uses exponential decay over a window of 8 samples. This allows the codec to adapt within a few messages when market conditions change.

### 2.4 Regime Detection

Every 64 price-bearing messages, the codec evaluates market regime:

```
avg_abs_price_residual = sum(|price_residual|) / 64
regime = (avg > 30) ? VOLATILE : CALM
```

The regime adjusts all k parameters simultaneously:

| Field | CALM k | VOLATILE k |
|-------|--------|-----------|
| Timestamp | 10 | 8 |
| Price | 3 | 7 |
| Quantity | 4 | 7 |
| Sequential ID | 3 | 5 |

## 3. Framing

Each compressed message is framed as:

```
[frame_byte: 1 byte] [payload: frame_byte bytes]
```

- `frame_byte` = 1-254: compressed payload of that length
- `frame_byte` = 0xFF: literal fallback (uncompressed)

The literal fallback ensures the compressed output is never larger than the raw input. This guarantees worst-case expansion of at most 1 byte per message.

## 4. State Synchronization

The encoder and decoder maintain identical state:
- Global: last timestamp, EMA, last instrument, regime, MFU table
- Per-field: last value, adaptive k parameters
- Per-instrument: last price, last quantity, last sequential ID, message count

State is initialized to zero. Both sides update state identically after processing each message. No out-of-band synchronization is needed — the state is fully determined by the message stream.

If a message is lost (detected by sequence number gap), the receiver requests retransmission. After gap fill, state is back in sync.

## 5. Results

### NASDAQ ITCH 5.0 (8M real messages)

| Metric | C (gcc -O2) | Rust (--release, LTO) |
|--------|------------|----------------------|
| Messages | 100,000 (synthetic) | 100,000 (synthetic) |
| Ratio | 2.71:1 | 2.37:1 |
| Encode latency | 51 ns/msg | **52 ns/msg** |
| Decode latency | 46 ns/msg | **39 ns/msg** |
| Roundtrip errors | 0 | 0 |

Both implementations use `always_inline` / `#[inline(always)]` on the entire hot path. The Rust implementation additionally uses hash-accelerated MFU lookup, direct-write BitWriter (zero intermediate buffer), and single-pass encode+commit. On real NASDAQ ITCH 5.0 data (8M messages, 32B avg), the C codec achieves 3.27:1 compression.

### Compression breakdown by technique

Contribution of each technique to the overall ratio (measured by disabling one at a time):

| Technique | Ratio without | Contribution |
|-----------|--------------|-------------|
| Baseline (raw) | 1.00:1 | — |
| + Tiered residual coding | ~2.0:1 | +100% |
| + Per-instrument prediction | ~2.4:1 | +20% |
| + Timestamp EMA | ~2.6:1 | +8% |
| + MFU instrument table | ~2.8:1 | +8% |
| + Stock symbol elimination | ~2.9:1 | +4% |
| + Penny/round-lot normalization | ~3.0:1 | +3% |
| + MPID table | ~3.1:1 | +3% |
| + Shares zero-residual flag | ~3.2:1 | +3% |
| + NOII/admin per-stock prediction | ~3.27:1 | +2% |

### Comparison to general-purpose compressors

On the same ITCH data:

| Codec | Ratio | Per-msg? | Throughput |
|-------|-------|----------|-----------|
| residc | 3.27:1 | Yes | 331 MB/s |
| LZ4 per-msg | 1.02:1 | Yes | 144 MB/s |
| LZ4 block | 1.80:1 | No | 538 MB/s |
| zstd block | ~2.5:1 | No | ~350 MB/s |

## 6. Related Work

- **FAST Protocol** (FIX Adapted for STreaming): Template-based delta encoding for financial data, standardized by FIX Trading Community. Achieved 2-4:1 with stop-bit coding (7 useful bits per byte). Being phased out: CME discontinued FAST feeds in 2023, replaced by SBE for low-latency. residc fills the compression niche FAST left behind, with better predictions (EMA, MFU, regime detection vs simple delta) and tighter coding (bit-level vs byte-level).

- **SBE** (Simple Binary Encoding): Fixed-layout binary encoding with zero compression. ~25ns encode via pointer cast. Industry standard for order entry at CME, Euronext, LSE. Optimal when bandwidth is unlimited (same-rack colo). residc complements SBE for bandwidth-constrained paths where 3x compression reduces total delivery time.

- **Cap'n Proto / FlatBuffers**: Zero-copy serialization formats. ~10-15ns encode, no compression. Similar trade-off to SBE: fastest possible encode, no size reduction.

- **Protocol Buffers**: Schema-driven serialization with varint encoding. ~1.3:1 on financial data. No cross-message prediction, no domain-specific strategies. Designed for general-purpose RPC, not financial streaming.

- **JPEG-LS** (ITU-T T.87): Prediction-residual coding for images using Golomb-Rice codes and context-adaptive k. Our tiered code and adaptive k are directly inspired by JPEG-LS, adapted for the different value distributions in financial data (Zipf instrument distribution, penny-aligned prices vs pixel gradients).

- **FLAC**: Prediction-residual coding for audio with Rice codes. Same core principle: predict from context, encode the error. FLAC uses linear prediction with fixed coefficients; residc uses domain-specific predictors per field type.

- **DPCM** (Differential Pulse-Code Modulation): The general signal processing technique that underlies all prediction-residual approaches. residc applies DPCM independently to each field in a structured message, with field-type-specific predictors.

## 7. Limitations

- **Higher encode latency than SBE**: SBE achieves ~25ns through zero-copy pointer cast. residc is ~51ns (C) / ~52ns (Rust) due to prediction + bit-level coding. On 10GbE same-rack links where bandwidth is free, SBE delivers lower total latency.

- **State dependency**: Messages must be decoded in order from a known state. Loss of a single message desynchronizes encoder and decoder. Recovery requires retransmission from last known-good state or full state reset. This is inherent to any streaming prediction approach (same limitation as FAST Protocol).

- **Memory footprint**: ~330KB per codec instance (16,384 instrument state slots + MFU table + field state). Configurable via `MAX_INSTRUMENTS` constant.

- **Domain-specific**: The prediction strategies assume financial data patterns (quasi-periodic timestamps, Zipf instrument distribution, mean-reverting prices, sequential IDs). Applying to non-financial domains would require different field types.

- **Schema agreement**: Both encoder and decoder must use the same schema. Schema evolution requires explicit versioning or negotiation.
