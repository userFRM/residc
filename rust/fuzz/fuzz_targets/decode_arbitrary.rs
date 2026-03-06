#![no_main]
use libfuzzer_sys::fuzz_target;
use residc::{Codec, FieldType, Schema};

/// Feed arbitrary bytes to the decoder.
/// Must not panic or crash on any input.
fuzz_target!(|data: &[u8]| {
    let schema = Schema::builder()
        .field("timestamp", FieldType::Timestamp)
        .field("instrument", FieldType::Instrument)
        .field("price", FieldType::Price)
        .field("quantity", FieldType::Quantity)
        .field("side", FieldType::Bool)
        .build();

    let mut dec = Codec::new(&schema);
    let _ = dec.decode(data);
});
