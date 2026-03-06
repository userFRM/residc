//! residc — FFI bindings to the C core prediction-residual compression codec.
//!
//! This crate wraps the C implementation via the SDK's opaque-handle API,
//! providing a safe, idiomatic Rust interface for encoding and decoding
//! financial market data messages.
//!
//! # Example
//!
//! ```
//! use residc::{Codec, FieldType};
//!
//! let fields = &[
//!     FieldType::Timestamp,
//!     FieldType::Instrument,
//!     FieldType::Price,
//!     FieldType::Quantity,
//!     FieldType::Enum,
//! ];
//!
//! let mut encoder = Codec::new(fields, None).unwrap();
//! let mut decoder = Codec::new(fields, None).unwrap();
//!
//! let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
//! let compressed = encoder.encode(&values).unwrap();
//! let decoded = decoder.decode(&compressed).unwrap();
//!
//! assert_eq!(&values[..], &decoded[..]);
//! ```

use std::fmt;
use std::ptr;

// ---------------------------------------------------------------------------
// FFI declarations matching sdk/residc_sdk.h
// ---------------------------------------------------------------------------

#[repr(C)]
struct residc_codec {
    _private: [u8; 0],
}

extern "C" {
    fn residc_codec_create(
        types: *const i32,
        ref_fields: *const i8,
        num_fields: i32,
    ) -> *mut residc_codec;

    fn residc_codec_destroy(codec: *mut residc_codec);

    fn residc_codec_encode(
        codec: *mut residc_codec,
        values: *const u64,
        out: *mut u8,
        capacity: i32,
    ) -> i32;

    fn residc_codec_decode(
        codec: *mut residc_codec,
        data: *const u8,
        data_len: i32,
        values: *mut u64,
    ) -> i32;

    fn residc_codec_snapshot(codec: *const residc_codec) -> *mut residc_codec;

    fn residc_codec_restore(codec: *mut residc_codec, snap: *const residc_codec);

    fn residc_codec_reset(codec: *mut residc_codec);

    fn residc_codec_seed_mfu(codec: *mut residc_codec, ids: *const u16, counts: *const u16, n: i32);
}

// ---------------------------------------------------------------------------
// Field types
// ---------------------------------------------------------------------------

/// Field types for defining a message schema.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum FieldType {
    Timestamp = 0,
    Instrument = 1,
    Price = 2,
    Quantity = 3,
    SequentialId = 4,
    Enum = 5,
    Bool = 6,
    Categorical = 7,
    Raw = 8,
    DeltaId = 9,
    DeltaPrice = 10,
    Computed = 11,
}

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

/// Errors returned by codec operations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// Codec creation failed (invalid schema or allocation failure).
    CreateFailed,
    /// Encoding failed (buffer too small or internal error).
    EncodeFailed,
    /// Decoding failed (corrupt data or state mismatch).
    DecodeFailed,
    /// Field count doesn't match the schema.
    FieldCountMismatch { expected: usize, got: usize },
    /// Too many fields (max 256).
    TooManyFields(usize),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::CreateFailed => write!(f, "codec creation failed"),
            Error::EncodeFailed => write!(f, "encode failed"),
            Error::DecodeFailed => write!(f, "decode failed"),
            Error::FieldCountMismatch { expected, got } => {
                write!(f, "expected {expected} values, got {got}")
            }
            Error::TooManyFields(n) => write!(f, "too many fields: {n} (max 256)"),
        }
    }
}

impl std::error::Error for Error {}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

/// A compression codec instance wrapping the C core.
///
/// Maintains synchronized prediction state — encode and decode calls
/// must be paired in the same order on both sides.
pub struct Codec {
    ptr: *mut residc_codec,
    num_fields: usize,
}

// SAFETY: The C codec has no thread-local or shared mutable state.
// Each instance is independent and can be sent across threads.
unsafe impl Send for Codec {}

impl Codec {
    /// Create a new codec with the given field types.
    ///
    /// `ref_fields` is an optional slice of reference field indices for
    /// `DeltaId` / `DeltaPrice` fields. Use -1 for non-delta fields.
    /// Pass `None` if there are no delta fields.
    pub fn new(fields: &[FieldType], ref_fields: Option<&[i8]>) -> Result<Self, Error> {
        let n = fields.len();
        if n > 256 {
            return Err(Error::TooManyFields(n));
        }

        let types: Vec<i32> = fields.iter().map(|f| *f as i32).collect();

        let ref_ptr = match ref_fields {
            Some(r) => r.as_ptr(),
            None => ptr::null(),
        };

        let ptr = unsafe { residc_codec_create(types.as_ptr(), ref_ptr, n as i32) };

        if ptr.is_null() {
            return Err(Error::CreateFailed);
        }

        Ok(Codec { ptr, num_fields: n })
    }

    /// Encode field values into compressed bytes.
    ///
    /// `values` must contain exactly `num_fields` elements (one `u64` per field).
    pub fn encode(&mut self, values: &[u64]) -> Result<Vec<u8>, Error> {
        if values.len() != self.num_fields {
            return Err(Error::FieldCountMismatch {
                expected: self.num_fields,
                got: values.len(),
            });
        }

        let mut buf = vec![0u8; 256];
        let len = unsafe {
            residc_codec_encode(
                self.ptr,
                values.as_ptr(),
                buf.as_mut_ptr(),
                buf.len() as i32,
            )
        };

        if len < 0 {
            return Err(Error::EncodeFailed);
        }

        buf.truncate(len as usize);
        Ok(buf)
    }

    /// Decode compressed bytes into field values.
    ///
    /// Returns a `Vec<u64>` with one element per field.
    pub fn decode(&mut self, data: &[u8]) -> Result<Vec<u64>, Error> {
        let mut values = vec![0u64; self.num_fields];
        let consumed = unsafe {
            residc_codec_decode(
                self.ptr,
                data.as_ptr(),
                data.len() as i32,
                values.as_mut_ptr(),
            )
        };

        if consumed < 0 {
            return Err(Error::DecodeFailed);
        }

        Ok(values)
    }

    /// Take a snapshot of the current codec state for gap recovery.
    pub fn snapshot(&self) -> Result<Snapshot, Error> {
        let snap = unsafe { residc_codec_snapshot(self.ptr) };
        if snap.is_null() {
            return Err(Error::CreateFailed);
        }
        Ok(Snapshot { ptr: snap })
    }

    /// Restore codec state from a snapshot.
    pub fn restore(&mut self, snap: &Snapshot) {
        unsafe { residc_codec_restore(self.ptr, snap.ptr) };
    }

    /// Reset codec to initial state.
    pub fn reset(&mut self) {
        unsafe { residc_codec_reset(self.ptr) };
    }

    /// Pre-seed the MFU (Most Frequently Used) instrument table.
    ///
    /// `ids` and `counts` must have the same length.
    /// Call identically on both encoder and decoder before any encode/decode.
    pub fn seed_mfu(&mut self, ids: &[u16], counts: &[u16]) {
        let n = ids.len().min(counts.len());
        unsafe {
            residc_codec_seed_mfu(self.ptr, ids.as_ptr(), counts.as_ptr(), n as i32);
        }
    }

    /// Number of fields in the schema.
    pub fn num_fields(&self) -> usize {
        self.num_fields
    }
}

impl Drop for Codec {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { residc_codec_destroy(self.ptr) };
        }
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

/// A saved codec state for gap recovery.
///
/// Created via [`Codec::snapshot()`]. Freed automatically on drop.
pub struct Snapshot {
    ptr: *mut residc_codec,
}

unsafe impl Send for Snapshot {}

impl Drop for Snapshot {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { residc_codec_destroy(self.ptr) };
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn quote_fields() -> Vec<FieldType> {
        vec![
            FieldType::Timestamp,
            FieldType::Instrument,
            FieldType::Price,
            FieldType::Quantity,
            FieldType::Enum,
        ]
    }

    #[test]
    fn roundtrip_single() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();
        let mut dec = Codec::new(&fields, None).unwrap();

        let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
        let compressed = enc.encode(&values).unwrap();
        let decoded = dec.decode(&compressed).unwrap();

        assert_eq!(&values[..], &decoded[..]);
    }

    #[test]
    fn roundtrip_many() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();
        let mut dec = Codec::new(&fields, None).unwrap();

        let base_ts: u64 = 34_200_000_000_000;
        for i in 0..1000u64 {
            let values = [
                base_ts + i * 1_000_000,
                42 + (i % 5),
                1_500_250 + (i % 100),
                100 + (i % 10),
                i % 3,
            ];
            let compressed = enc.encode(&values).unwrap();
            let decoded = dec.decode(&compressed).unwrap();
            assert_eq!(&values[..], &decoded[..], "mismatch at message {i}");
        }
    }

    #[test]
    fn snapshot_restore() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();
        let mut dec = Codec::new(&fields, None).unwrap();

        // Send some messages to build state
        let base_ts: u64 = 34_200_000_000_000;
        for i in 0..100u64 {
            let values = [base_ts + i * 1_000_000, 42, 1_500_250, 100, 0];
            let compressed = enc.encode(&values).unwrap();
            dec.decode(&compressed).unwrap();
        }

        // Snapshot
        let snap_enc = enc.snapshot().unwrap();
        let snap_dec = dec.snapshot().unwrap();

        // Send more messages
        for i in 100..200u64 {
            let values = [base_ts + i * 1_000_000, 42, 1_500_250, 100, 0];
            enc.encode(&values).unwrap();
        }

        // Restore and verify roundtrip still works
        enc.restore(&snap_enc);
        dec.restore(&snap_dec);

        for i in 100..200u64 {
            let values = [base_ts + i * 1_000_000, 42, 1_500_250, 100, 0];
            let compressed = enc.encode(&values).unwrap();
            let decoded = dec.decode(&compressed).unwrap();
            assert_eq!(&values[..], &decoded[..]);
        }
    }

    #[test]
    fn reset_works() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();
        let mut dec = Codec::new(&fields, None).unwrap();

        // Build state
        for i in 0..50u64 {
            let values = [34_200_000_000_000 + i * 1_000_000, 42, 1_500_250, 100, 0];
            let compressed = enc.encode(&values).unwrap();
            dec.decode(&compressed).unwrap();
        }

        // Reset both
        enc.reset();
        dec.reset();

        // Should still roundtrip correctly from fresh state
        let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
        let compressed = enc.encode(&values).unwrap();
        let decoded = dec.decode(&compressed).unwrap();
        assert_eq!(&values[..], &decoded[..]);
    }

    #[test]
    fn field_count_mismatch() {
        let fields = quote_fields();
        let mut codec = Codec::new(&fields, None).unwrap();

        let result = codec.encode(&[1, 2, 3]); // too few
        assert!(matches!(
            result,
            Err(Error::FieldCountMismatch {
                expected: 5,
                got: 3
            })
        ));
    }

    #[test]
    fn seed_mfu() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();
        let mut dec = Codec::new(&fields, None).unwrap();

        let ids = [42u16, 100, 200];
        let counts = [1000u16, 500, 250];
        enc.seed_mfu(&ids, &counts);
        dec.seed_mfu(&ids, &counts);

        let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
        let compressed = enc.encode(&values).unwrap();
        let decoded = dec.decode(&compressed).unwrap();
        assert_eq!(&values[..], &decoded[..]);
    }

    #[test]
    fn compression_ratio() {
        let fields = quote_fields();
        let mut enc = Codec::new(&fields, None).unwrap();

        let base_ts: u64 = 34_200_000_000_000;
        let mut total_compressed = 0usize;
        let n = 1000;

        for i in 0..n as u64 {
            let values = [
                base_ts + i * 1_000_000,
                42 + (i % 5),
                1_500_250 + (i % 100),
                100 + (i % 10),
                i % 3,
            ];
            let compressed = enc.encode(&values).unwrap();
            total_compressed += compressed.len();
        }

        let raw_size = 22 * n; // 8+2+4+4+1 = 19 bytes per msg, but SDK uses 5*8=40 u64s → 22 raw
        let ratio = raw_size as f64 / total_compressed as f64;
        // Should achieve at least 2x compression on this data
        assert!(
            ratio > 2.0,
            "compression ratio {ratio:.2}x is below 2x threshold"
        );
    }
}
