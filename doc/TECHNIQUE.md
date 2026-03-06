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
Tier 0:  0  + k bits           values [0, 2^k)         cost: 1+k bits
Tier 1:  10 + (k+6) bits       values [0, 2^(k+6))     cost: 2+k+6 bits
Tier 2:  110 + (k+12) bits     values [0, 2^(k+12))    cost: 3+k+12 bits
Tier 3:  1110 + (k+20) bits    values [0, 2^(k+20))    cost: 4+k+20 bits
Tier 4:  1111 + 64 raw bits    any value                cost: 4+64 bits
```

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

| Metric | Value |
|--------|-------|
| Messages | 8,015,810 |
| Raw data | 261.7 MB |
| Compressed | 78.99 MB |
| Ratio | 3.27:1 |
| Avg raw msg | 32.2 bytes |
| Avg compressed msg | 9.9 bytes |
| Encode throughput | 331 MB/s |
| Decode throughput | 348 MB/s |
| Roundtrip errors | 0 |

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

- **FAST Protocol** (FIX Adapted for STreaming): Template-based delta encoding for financial data, standardized by FIX Trading Community. Achieves 2-4:1 but uses complex template definitions and is being phased out in favor of SBE.

- **SBE** (Simple Binary Encoding): Fixed-layout binary encoding with zero compression. Optimized for ultra-low latency (~25ns) direct memory access. Industry standard for order entry at CME, Euronext, etc.

- **JPEG-LS**: Prediction-residual coding for images using Golomb-Rice codes. Our tiered code is inspired by this approach but adapted for the different value distributions in financial data.

- **FLAC**: Prediction-residual coding for audio with Rice codes. Similar principle, different domain.

- **Protocol Buffers / FlatBuffers**: Schema-driven serialization with varint encoding. Not domain-specific; no cross-message prediction.

## 7. Limitations

- **Not faster than SBE**: SBE achieves ~25ns encode/decode through zero-copy direct memory access. residc is ~100ns due to bit manipulation.

- **State dependency**: Messages can only be decoded in order (or from a known state). This is inherent to any streaming prediction approach.

- **Domain-specific**: The prediction strategies are designed for financial data patterns. Applying residc to non-financial data (logs, sensor data) would require different field types.

- **Schema must be known**: Both encoder and decoder must agree on the schema. Schema evolution requires versioning.
