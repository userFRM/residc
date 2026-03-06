#![no_main]
use libfuzzer_sys::fuzz_target;
use residc::{Codec, FieldType, Message, Schema};

fuzz_target!(|data: &[u8]| {
    let schema = Schema::builder()
        .field("timestamp", FieldType::Timestamp)
        .field("instrument", FieldType::Instrument)
        .field("price", FieldType::Price)
        .field("quantity", FieldType::Quantity)
        .field("side", FieldType::Bool)
        .build();

    let mut enc = Codec::new(&schema);
    let mut dec = Codec::new(&schema);

    // Each message consumes 19 bytes: 8 (ts) + 2 (inst) + 4 (price) + 4 (qty) + 1 (side)
    let msg_bytes = 19;
    let num_msgs = data.len() / msg_bytes;

    for i in 0..num_msgs {
        let d = &data[i * msg_bytes..];
        let ts = u64::from_le_bytes(d[0..8].try_into().unwrap());
        let instrument = u16::from_le_bytes(d[8..10].try_into().unwrap()) % 16384;
        let price = u32::from_le_bytes(d[10..14].try_into().unwrap());
        let qty = u32::from_le_bytes(d[14..18].try_into().unwrap());
        let side = d[18] & 1;

        let msg = Message::new()
            .set(0, ts)
            .set(1, instrument as u64)
            .set(2, price as u64)
            .set(3, qty as u64)
            .set(4, side as u64);

        let mut buf = [0u8; 128];
        let len = enc.encode(&msg, &mut buf).unwrap();
        let decoded = dec.decode(&buf[..len]).unwrap();

        assert_eq!(decoded.get(0), ts, "timestamp mismatch at msg {}", i);
        assert_eq!(decoded.get(1), instrument as u64, "instrument mismatch at msg {}", i);
        assert_eq!(decoded.get(2), price as u64, "price mismatch at msg {}", i);
        assert_eq!(decoded.get(3), qty as u64, "quantity mismatch at msg {}", i);
        assert_eq!(decoded.get(4), side as u64, "side mismatch at msg {}", i);
    }
});
