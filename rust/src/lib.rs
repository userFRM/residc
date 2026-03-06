//! # residc
//!
//! **Per-message prediction-residual compression for financial data.**
//!
//! `no_std`, zero-dependency, zero-allocation encode/decode.
//!
//! Compresses financial messages (quotes, orders, trades) 3-4x by predicting
//! each field from context and encoding only the prediction error. Unlike
//! general-purpose compressors (LZ4, zstd), it works on individual messages —
//! no blocks, no buffering, per-message random access.
//!
//! ## Quick Start
//!
//! ```rust
//! use residc::{Schema, FieldType, Codec, Message};
//!
//! let schema = Schema::builder()
//!     .field("timestamp", FieldType::Timestamp)
//!     .field("instrument", FieldType::Instrument)
//!     .field("price", FieldType::Price)
//!     .field("quantity", FieldType::Quantity)
//!     .field("side", FieldType::Bool)
//!     .build();
//!
//! let mut enc = Codec::new(&schema);
//! let mut dec = Codec::new(&schema);
//!
//! let msg = Message::new()
//!     .set(0, 34_200_000_000_000u64)  // timestamp
//!     .set(1, 42u64)                   // instrument
//!     .set(2, 1_500_250u64)            // price
//!     .set(3, 100u64)                  // quantity
//!     .set(4, 0u64);                   // side
//!
//! let mut buf = [0u8; 64];
//! let len = enc.encode(&msg, &mut buf).unwrap();
//! let decoded = dec.decode(&buf[..len]).unwrap();
//! assert_eq!(decoded.get(2), 1_500_250);
//! ```

#![no_std]

mod bits;
mod mfu;
mod residual;

pub const MAX_FIELDS: usize = 32;
pub const MAX_INSTRUMENTS: usize = 16384;
pub const MFU_SIZE: usize = 64;
pub const MFU_INDEX_BITS: u32 = 6;
pub const ADAPT_WINDOW: u32 = 8;
pub const REGIME_WINDOW: u32 = 64;
pub const FRAME_LITERAL: u8 = 0xFF;

// ================================================================
// Field types
// ================================================================

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FieldType {
    /// Nanosecond timestamp — EMA gap prediction + adaptive k
    Timestamp,
    /// Instrument/security ID — MFU table (uint16)
    Instrument,
    /// Price (fixed-point uint32) — per-instrument prediction + penny normalization
    Price,
    /// Quantity (uint32) — per-instrument prediction + zero-residual flag
    Quantity,
    /// Sequential ID (uint64) — delta from last
    SequentialId,
    /// Small enum (uint8) — same-as-last flag
    Enum,
    /// Boolean — 1 bit
    Bool,
    /// Categorical (uint32) — same-as-last flag
    Categorical,
    /// Raw bytes — no prediction
    Raw { bytes: u8 },
    /// Delta from another field in the same message
    DeltaId { ref_field: u8 },
    /// Price delta from another price field
    DeltaPrice { ref_field: u8 },
    /// Computed (not transmitted — 0 bits)
    Computed,
}

// ================================================================
// Schema (fixed-size, no heap)
// ================================================================

#[derive(Clone, Copy, Debug)]
pub struct FieldDef {
    pub name: &'static str,
    pub field_type: FieldType,
}

const EMPTY_FIELD: FieldDef = FieldDef { name: "", field_type: FieldType::Computed };

#[derive(Clone, Debug)]
pub struct Schema {
    fields: [FieldDef; MAX_FIELDS],
    num_fields: usize,
    raw_size: usize,
}

pub struct SchemaBuilder {
    fields: [FieldDef; MAX_FIELDS],
    num_fields: usize,
}

impl Schema {
    pub fn builder() -> SchemaBuilder {
        SchemaBuilder {
            fields: [EMPTY_FIELD; MAX_FIELDS],
            num_fields: 0,
        }
    }

    #[inline]
    pub fn num_fields(&self) -> usize {
        self.num_fields
    }

    #[inline]
    pub fn raw_size(&self) -> usize {
        self.raw_size
    }

    #[inline]
    pub fn field(&self, index: usize) -> &FieldDef {
        &self.fields[index]
    }
}

impl SchemaBuilder {
    pub fn field(mut self, name: &'static str, field_type: FieldType) -> Self {
        assert!(self.num_fields < MAX_FIELDS, "too many fields");
        self.fields[self.num_fields] = FieldDef { name, field_type };
        self.num_fields += 1;
        self
    }

    pub fn build(self) -> Schema {
        let mut raw_size = 0;
        for i in 0..self.num_fields {
            raw_size += field_raw_bytes(self.fields[i].field_type);
        }
        Schema {
            fields: self.fields,
            num_fields: self.num_fields,
            raw_size,
        }
    }
}

fn field_raw_bytes(ft: FieldType) -> usize {
    match ft {
        FieldType::Timestamp => 8,
        FieldType::Instrument => 2,
        FieldType::Price => 4,
        FieldType::Quantity => 4,
        FieldType::SequentialId => 8,
        FieldType::Enum => 1,
        FieldType::Bool => 1,
        FieldType::Categorical => 4,
        FieldType::Raw { bytes } => bytes as usize,
        FieldType::DeltaId { .. } => 8,
        FieldType::DeltaPrice { .. } => 4,
        FieldType::Computed => 0,
    }
}

// ================================================================
// Message
// ================================================================

#[derive(Clone, Debug)]
pub struct Message {
    pub values: [u64; MAX_FIELDS],
}

impl Message {
    pub fn new() -> Self {
        Self { values: [0u64; MAX_FIELDS] }
    }

    #[inline]
    pub fn set(mut self, index: usize, value: u64) -> Self {
        self.values[index] = value;
        self
    }

    #[inline]
    pub fn set_mut(&mut self, index: usize, value: u64) {
        self.values[index] = value;
    }

    #[inline]
    pub fn get(&self, index: usize) -> u64 {
        self.values[index]
    }
}

impl Default for Message {
    fn default() -> Self {
        Self::new()
    }
}

// ================================================================
// Codec state (fully stack-allocated, ~330KB)
// ================================================================

#[derive(Clone, Copy)]
struct InstrumentState {
    last_price: u32,
    last_qty: u32,
    msg_count: u32,
    last_seq_id: u64,
}

impl InstrumentState {
    const ZERO: Self = Self { last_price: 0, last_qty: 0, msg_count: 0, last_seq_id: 0 };
}

#[derive(Clone, Copy)]
struct FieldState {
    last_value: u64,
    adapt_sum: u64,
    adapt_count: u32,
}

impl FieldState {
    const ZERO: Self = Self { last_value: 0, adapt_sum: 0, adapt_count: 0 };
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Regime { Calm, Volatile }

pub struct Codec {
    schema: Schema,
    msg_count: u64,

    // Timestamp
    last_timestamp: u64,
    ts_gap_ema: i64,
    ts_adapt_sum: u64,
    ts_adapt_count: u32,

    // Instrument
    last_instrument: u16,
    mfu: mfu::MfuTable,
    mfu_decay_counter: u32,

    // Regime
    regime: Regime,
    regime_counter: u32,
    recent_abs_price_sum: u32,

    // Per-field
    field_state: [FieldState; MAX_FIELDS],

    // Per-instrument (fixed array, no heap)
    instruments: [InstrumentState; MAX_INSTRUMENTS],
}

impl Codec {
    pub fn new(schema: &Schema) -> Self {
        Self {
            schema: schema.clone(),
            msg_count: 0,
            last_timestamp: 0,
            ts_gap_ema: 0,
            ts_adapt_sum: 0,
            ts_adapt_count: 0,
            last_instrument: 0,
            mfu: mfu::MfuTable::new(),
            mfu_decay_counter: 0,
            regime: Regime::Calm,
            regime_counter: 0,
            recent_abs_price_sum: 0,
            field_state: [FieldState::ZERO; MAX_FIELDS],
            instruments: [InstrumentState::ZERO; MAX_INSTRUMENTS],
        }
    }

    pub fn reset(&mut self) {
        *self = Self::new(&self.schema);
    }

    #[inline(always)]
    fn k_ts(&self) -> u32 {
        if self.regime == Regime::Volatile { 8 } else { 10 }
    }

    #[inline(always)]
    fn k_price(&self) -> u32 {
        if self.regime == Regime::Volatile { 7 } else { 3 }
    }

    #[inline(always)]
    fn k_qty(&self) -> u32 {
        if self.regime == Regime::Volatile { 7 } else { 4 }
    }

    #[inline(always)]
    fn k_seq(&self) -> u32 {
        if self.regime == Regime::Volatile { 5 } else { 3 }
    }

    #[inline(always)]
    fn instrument_state(&self, id: u16) -> &InstrumentState {
        // SAFETY: id is masked to MAX_INSTRUMENTS range
        unsafe { self.instruments.get_unchecked((id as usize) & (MAX_INSTRUMENTS - 1)) }
    }

    #[inline(always)]
    fn instrument_state_mut(&mut self, id: u16) -> &mut InstrumentState {
        unsafe { self.instruments.get_unchecked_mut((id as usize) & (MAX_INSTRUMENTS - 1)) }
    }

    // ============================================================
    // Encode
    // ============================================================

    pub fn encode(&mut self, msg: &Message, out: &mut [u8]) -> Result<usize, &'static str> {
        if out.len() < 2 { return Err("buffer too small"); }

        // Single-pass: encode fields + update state in one loop
        let payload_len = {
            let mut bw = bits::BitWriter::new(&mut out[1..]);
            self.encode_and_commit(msg, &mut bw);
            bw.finish()
        };

        let raw_size = self.schema.raw_size();

        if payload_len >= raw_size || payload_len >= 254 {
            // State already committed — just overwrite with literal bytes
            let total = 1 + raw_size;
            if out.len() < total { return Err("buffer too small for literal"); }
            out[0] = FRAME_LITERAL;
            let mut pos = 1;
            for fi in 0..self.schema.num_fields {
                let fd = &self.schema.fields[fi];
                if matches!(fd.field_type, FieldType::Computed) { continue; }
                let val = msg.values[fi];
                let bytes = field_raw_bytes(fd.field_type);
                for b in (0..bytes).rev() {
                    out[pos] = (val >> (b * 8)) as u8;
                    pos += 1;
                }
            }
            return Ok(pos);
        }

        out[0] = payload_len as u8;
        Ok(1 + payload_len)
    }

    /// Encode all fields and update state in a single pass.
    /// Eliminates the second loop through fields that commit_state does separately.
    #[inline(always)]
    fn encode_and_commit(&mut self, msg: &Message, bw: &mut bits::BitWriter<'_>) {
        let mut instrument_id = self.last_instrument;
        // Copy instrument state to avoid holding a borrow on self
        let mut is = InstrumentState::ZERO;
        // Cache regime-dependent k values — must be consistent with decoder
        // which sees pre-commit regime for all fields
        let k_ts = self.k_ts();
        let k_price = self.k_price();
        let k_qty = self.k_qty();
        let k_seq = self.k_seq();

        for fi in 0..self.schema.num_fields {
            // SAFETY: fi < num_fields <= MAX_FIELDS
            let fd = unsafe { *self.schema.fields.get_unchecked(fi) };
            let val = unsafe { *msg.values.get_unchecked(fi) };

            match fd.field_type {
                FieldType::Timestamp => {
                    let gap = val.wrapping_sub(self.last_timestamp) as i64;
                    let predicted_gap = (self.ts_gap_ema >> 16).max(0);
                    let k = residual::adaptive_k(
                        self.ts_adapt_sum, self.ts_adapt_count,
                        k_ts, k_ts + 10,
                    );
                    let res = gap - predicted_gap;
                    residual::encode(bw, res, k);

                    // State update
                    let zz = residual::zigzag_enc(res);
                    residual::adaptive_update(
                        &mut self.ts_adapt_sum, &mut self.ts_adapt_count, zz,
                    );
                    let gap_q16 = gap << 16;
                    self.ts_gap_ema += (gap_q16 - self.ts_gap_ema) >> 2;
                    self.last_timestamp = val;
                }

                FieldType::Instrument => {
                    let id = val as u16;
                    instrument_id = id;
                    is = *self.instrument_state(id);

                    if id == self.last_instrument && self.msg_count > 0 {
                        bw.write(0, 1);
                    } else {
                        match self.mfu.lookup(id) {
                            Some(idx) => {
                                bw.write(0b10 << MFU_INDEX_BITS | idx as u64, 2 + MFU_INDEX_BITS);
                            }
                            None => {
                                bw.write((0b11 << 14) | id as u64, 2 + 14);
                            }
                        }
                    }

                    // State update
                    self.mfu.update(id);
                    self.mfu_decay_counter += 1;
                    if self.mfu_decay_counter >= 10000 {
                        self.mfu.decay();
                        self.mfu_decay_counter = 0;
                    }
                    self.last_instrument = id;
                }

                FieldType::Price => {
                    let price = val as u32;
                    let predicted = if is.msg_count > 0 { is.last_price } else { 0 };
                    let res = price as i64 - predicted as i64;
                    let k = k_price;

                    if price % 100 == 0 && predicted % 100 == 0
                        && (price > 0 || predicted > 0)
                    {
                        bw.write(0, 1);
                        residual::encode(bw, res / 100, k);
                    } else {
                        bw.write(1, 1);
                        residual::encode(bw, res, k);
                    }

                    // State update
                    let abs_res = (val as i64 - predicted as i64).unsigned_abs() as u32;
                    self.recent_abs_price_sum += abs_res;
                    self.regime_counter += 1;
                    if self.regime_counter >= REGIME_WINDOW {
                        let avg = self.recent_abs_price_sum / REGIME_WINDOW;
                        self.regime = if avg > 30 { Regime::Volatile } else { Regime::Calm };
                        self.recent_abs_price_sum = 0;
                        self.regime_counter = 0;
                    }
                    self.instrument_state_mut(instrument_id).last_price = val as u32;
                }

                FieldType::Quantity => {
                    let qty = val as u32;
                    let predicted = if is.msg_count > 0 { is.last_qty } else { 100 };
                    let res = qty as i64 - predicted as i64;
                    let k = k_qty;

                    if res == 0 {
                        bw.write(0, 1);
                    } else if qty % 100 == 0 && predicted % 100 == 0 {
                        bw.write(0b10, 2);
                        residual::encode(bw, res / 100, k);
                    } else {
                        bw.write(0b11, 2);
                        residual::encode(bw, res, k);
                    }

                    // State update
                    self.instrument_state_mut(instrument_id).last_qty = val as u32;
                }

                FieldType::SequentialId => {
                    let inst_seq = is.last_seq_id;
                    let fs_val = self.field_state[fi].last_value;
                    let fs_sum = self.field_state[fi].adapt_sum;
                    let fs_count = self.field_state[fi].adapt_count;
                    let predicted = if inst_seq > 0 { inst_seq } else { fs_val };
                    let delta = val.wrapping_sub(predicted) as i64;
                    let k = residual::adaptive_k(
                        fs_sum, fs_count,
                        k_seq, k_seq + 10,
                    );
                    residual::encode(bw, delta, k);

                    // State update
                    let zz = residual::zigzag_enc(delta);
                    let fs = &mut self.field_state[fi];
                    residual::adaptive_update(&mut fs.adapt_sum, &mut fs.adapt_count, zz);
                    fs.last_value = val;
                    self.instrument_state_mut(instrument_id).last_seq_id = val;
                }

                FieldType::Enum => {
                    if val == self.field_state[fi].last_value && self.msg_count > 0 {
                        bw.write(0, 1);
                    } else {
                        bw.write(1, 1);
                        bw.write(val, 8);
                    }
                    self.field_state[fi].last_value = val;
                }

                FieldType::Bool => {
                    bw.write(val & 1, 1);
                }

                FieldType::Categorical => {
                    if val == self.field_state[fi].last_value && self.msg_count > 0 {
                        bw.write(0, 1);
                    } else {
                        bw.write(1, 1);
                        bw.write(val >> 16, 16);
                        bw.write(val & 0xFFFF, 16);
                    }
                    self.field_state[fi].last_value = val;
                }

                FieldType::Raw { bytes } => {
                    bw.write(val, bytes as u32 * 8);
                }

                FieldType::DeltaId { ref_field } => {
                    let ref_val = msg.values[ref_field as usize];
                    let delta = val.wrapping_sub(ref_val) as i64;
                    residual::encode(bw, delta, k_seq);
                    self.field_state[fi].last_value = val;
                }

                FieldType::DeltaPrice { ref_field } => {
                    let ref_val = msg.values[ref_field as usize];
                    let delta = val as i64 - ref_val as i64;
                    residual::encode(bw, delta, k_price);
                }

                FieldType::Computed => {}
            }
        }

        self.instrument_state_mut(instrument_id).msg_count += 1;
        self.msg_count += 1;
    }

    // ============================================================
    // Decode
    // ============================================================

    pub fn decode(&mut self, input: &[u8]) -> Result<Message, &'static str> {
        if input.is_empty() { return Err("empty input"); }

        let frame = input[0];
        let mut msg = Message::new();

        if frame == FRAME_LITERAL {
            let mut pos = 1;
            for fi in 0..self.schema.num_fields {
                let fd = &self.schema.fields[fi];
                if matches!(fd.field_type, FieldType::Computed) { continue; }
                let bytes = field_raw_bytes(fd.field_type);
                let mut val = 0u64;
                for b in (0..bytes).rev() {
                    if pos >= input.len() { return Err("truncated literal"); }
                    val |= (input[pos] as u64) << (b * 8);
                    pos += 1;
                }
                msg.values[fi] = val;
            }
            self.commit_state(&msg);
            return Ok(msg);
        }

        let payload_len = frame as usize;
        if 1 + payload_len > input.len() { return Err("truncated payload"); }

        let mut br = bits::BitReader::new(&input[1..1 + payload_len]);
        self.decode_fields(&mut br, &mut msg);
        self.commit_state(&msg);
        Ok(msg)
    }

    #[inline(always)]
    fn decode_fields(&self, br: &mut bits::BitReader, msg: &mut Message) {
        let mut is: &InstrumentState = &InstrumentState::ZERO;

        for fi in 0..self.schema.num_fields {
            // SAFETY: fi < num_fields <= MAX_FIELDS
            let fd = unsafe { self.schema.fields.get_unchecked(fi) };
            let val: u64 = match fd.field_type {
                FieldType::Timestamp => {
                    let predicted_gap = (self.ts_gap_ema >> 16).max(0);
                    let k = residual::adaptive_k(
                        self.ts_adapt_sum, self.ts_adapt_count,
                        self.k_ts(), self.k_ts() + 10,
                    );
                    let res = residual::decode(br, k);
                    let gap = res + predicted_gap;
                    self.last_timestamp.wrapping_add(gap as u64)
                }

                FieldType::Instrument => {
                    let id = if br.read_bit() == 0 {
                        self.last_instrument
                    } else if br.read_bit() == 0 {
                        let idx = br.read(MFU_INDEX_BITS) as usize;
                        self.mfu.entries[idx].id
                    } else {
                        br.read(14) as u16
                    };
                    is = self.instrument_state(id);
                    id as u64
                }

                FieldType::Price => {
                    let predicted = if is.msg_count > 0 { is.last_price } else { 0 };
                    let k = self.k_price();
                    let mode = br.read_bit();
                    let res = residual::decode(br, k);
                    if mode == 0 {
                        ((predicted / 100) as i64 + res) as u64 * 100
                    } else {
                        (predicted as i64 + res) as u64
                    }
                }

                FieldType::Quantity => {
                    let predicted = if is.msg_count > 0 { is.last_qty } else { 100 };
                    let k = self.k_qty();
                    if br.read_bit() == 0 {
                        predicted as u64
                    } else {
                        let mode = br.read_bit();
                        let res = residual::decode(br, k);
                        if mode == 0 {
                            ((predicted / 100) as i64 + res) as u64 * 100
                        } else {
                            (predicted as i64 + res) as u64
                        }
                    }
                }

                FieldType::SequentialId => {
                    let fs = &self.field_state[fi];
                    let predicted = if is.last_seq_id > 0 { is.last_seq_id } else { fs.last_value };
                    let k = residual::adaptive_k(
                        fs.adapt_sum, fs.adapt_count,
                        self.k_seq(), self.k_seq() + 10,
                    );
                    let delta = residual::decode(br, k);
                    predicted.wrapping_add(delta as u64)
                }

                FieldType::Enum => {
                    let fs = &self.field_state[fi];
                    if br.read_bit() == 0 {
                        fs.last_value
                    } else {
                        br.read(8)
                    }
                }

                FieldType::Bool => br.read(1),

                FieldType::Categorical => {
                    let fs = &self.field_state[fi];
                    if br.read_bit() == 0 {
                        fs.last_value
                    } else {
                        let hi = br.read(16);
                        let lo = br.read(16);
                        (hi << 16) | lo
                    }
                }

                FieldType::Raw { bytes } => br.read(bytes as u32 * 8),

                FieldType::DeltaId { ref_field } => {
                    let ref_val = msg.values[ref_field as usize];
                    let delta = residual::decode(br, self.k_seq());
                    ref_val.wrapping_add(delta as u64)
                }

                FieldType::DeltaPrice { ref_field } => {
                    let ref_val = msg.values[ref_field as usize];
                    let delta = residual::decode(br, self.k_price());
                    (ref_val as i64 + delta) as u64
                }

                FieldType::Computed => continue,
            };

            // SAFETY: fi < num_fields <= MAX_FIELDS
            unsafe { *msg.values.get_unchecked_mut(fi) = val; }
        }
    }

    // ============================================================
    // State commit (called after successful encode or decode)
    // ============================================================

    #[inline(always)]
    fn commit_state(&mut self, msg: &Message) {
        let mut instrument_id = self.last_instrument;

        for fi in 0..self.schema.num_fields {
            // SAFETY: fi < num_fields <= MAX_FIELDS
            let fd = unsafe { *self.schema.fields.get_unchecked(fi) };
            let val = unsafe { *msg.values.get_unchecked(fi) };

            match fd.field_type {
                FieldType::Timestamp => {
                    let gap = val.wrapping_sub(self.last_timestamp) as i64;
                    let res = gap - (self.ts_gap_ema >> 16).max(0);
                    let zz = residual::zigzag_enc(res);

                    residual::adaptive_update(
                        &mut self.ts_adapt_sum, &mut self.ts_adapt_count, zz,
                    );

                    let gap_q16 = gap << 16;
                    self.ts_gap_ema += (gap_q16 - self.ts_gap_ema) >> 2;
                    self.last_timestamp = val;
                }

                FieldType::Instrument => {
                    instrument_id = val as u16;
                    self.mfu.update(instrument_id);
                    self.mfu_decay_counter += 1;
                    if self.mfu_decay_counter >= 10000 {
                        self.mfu.decay();
                        self.mfu_decay_counter = 0;
                    }
                    self.last_instrument = instrument_id;
                }

                FieldType::Price => {
                    let is = self.instrument_state_mut(instrument_id);
                    let predicted = if is.msg_count > 0 { is.last_price } else { 0 };
                    let res = (val as i64 - predicted as i64).unsigned_abs() as u32;
                    self.recent_abs_price_sum += res;
                    self.regime_counter += 1;
                    if self.regime_counter >= REGIME_WINDOW {
                        let avg = self.recent_abs_price_sum / REGIME_WINDOW;
                        self.regime = if avg > 30 { Regime::Volatile } else { Regime::Calm };
                        self.recent_abs_price_sum = 0;
                        self.regime_counter = 0;
                    }
                    self.instrument_state_mut(instrument_id).last_price = val as u32;
                }

                FieldType::Quantity => {
                    self.instrument_state_mut(instrument_id).last_qty = val as u32;
                }

                FieldType::SequentialId => {
                    let inst_seq = self.instrument_state(instrument_id).last_seq_id;
                    let fs = &mut self.field_state[fi];
                    let predicted = if inst_seq > 0 { inst_seq } else { fs.last_value };
                    let delta = val.wrapping_sub(predicted) as i64;
                    let zz = residual::zigzag_enc(delta);
                    residual::adaptive_update(&mut fs.adapt_sum, &mut fs.adapt_count, zz);
                    fs.last_value = val;
                    self.instrument_state_mut(instrument_id).last_seq_id = val;
                }

                FieldType::Enum | FieldType::Categorical | FieldType::DeltaId { .. } => {
                    self.field_state[fi].last_value = val;
                }

                _ => {}
            }
        }

        self.instrument_state_mut(instrument_id).msg_count += 1;
        self.msg_count += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_basic() {
        let schema = Schema::builder()
            .field("timestamp", FieldType::Timestamp)
            .field("instrument", FieldType::Instrument)
            .field("price", FieldType::Price)
            .field("quantity", FieldType::Quantity)
            .field("side", FieldType::Bool)
            .build();

        let mut enc = Codec::new(&schema);
        let mut dec = Codec::new(&schema);

        let mut ts = 34_200_000_000_000u64;
        let mut errors = 0;

        for i in 0..1000u64 {
            ts += 1000 + (i * 37 % 50000);
            let instrument = (i % 50) as u16;
            let price = 1_500_000 + ((i * 7) % 2000) as u32;
            let qty = ((1 + i % 20) * 100) as u32;
            let side = (i % 2) as u8;

            let msg = Message::new()
                .set(0, ts)
                .set(1, instrument as u64)
                .set(2, price as u64)
                .set(3, qty as u64)
                .set(4, side as u64);

            let mut buf = [0u8; 64];
            let len = enc.encode(&msg, &mut buf).unwrap();
            let decoded = dec.decode(&buf[..len]).unwrap();

            if decoded.get(0) != ts
                || decoded.get(1) != instrument as u64
                || decoded.get(2) != price as u64
                || decoded.get(3) != qty as u64
                || decoded.get(4) != side as u64
            {
                errors += 1;
            }
        }
        assert_eq!(errors, 0, "roundtrip errors");
    }

    #[test]
    fn compression_ratio() {
        let schema = Schema::builder()
            .field("timestamp", FieldType::Timestamp)
            .field("instrument", FieldType::Instrument)
            .field("price", FieldType::Price)
            .field("quantity", FieldType::Quantity)
            .field("side", FieldType::Bool)
            .build();

        let mut enc = Codec::new(&schema);
        let raw_size = schema.raw_size();
        let mut total_raw = 0usize;
        let mut total_comp = 0usize;
        let mut ts = 34_200_000_000_000u64;

        for i in 0..10_000u64 {
            ts += 1000 + (i * 37 % 50000);
            let msg = Message::new()
                .set(0, ts)
                .set(1, (i % 50) as u64)
                .set(2, (1_500_000 + (i * 7 % 2000)) as u64)
                .set(3, ((1 + i % 20) * 100) as u64)
                .set(4, (i % 2) as u64);

            let mut buf = [0u8; 64];
            let len = enc.encode(&msg, &mut buf).unwrap();
            total_raw += raw_size;
            total_comp += len;
        }

        let ratio = total_raw as f64 / total_comp as f64;
        assert!(ratio > 1.5, "compression ratio should be > 1.5, got {:.2}", ratio);
    }
}
