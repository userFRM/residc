use criterion::{Criterion, criterion_group, criterion_main};
use residc::{Codec, FieldType, Message, Schema};

fn build_schema() -> Schema {
    Schema::builder()
        .field("timestamp", FieldType::Timestamp)
        .field("instrument", FieldType::Instrument)
        .field("price", FieldType::Price)
        .field("quantity", FieldType::Quantity)
        .field("side", FieldType::Bool)
        .build()
}

fn make_messages(n: u64) -> Vec<Message> {
    let mut ts = 34_200_000_000_000u64;
    (0..n)
        .map(|i| {
            ts += 1000 + (i * 37 % 50000);
            Message::new()
                .set(0, ts)
                .set(1, (i % 50) as u64)
                .set(2, (1_500_000 + (i * 7 % 2000)) as u64)
                .set(3, ((1 + i % 20) * 100) as u64)
                .set(4, (i % 2) as u64)
        })
        .collect()
}

fn bench_encode(c: &mut Criterion) {
    let schema = build_schema();
    let msgs = make_messages(10_000);

    c.bench_function("encode", |b| {
        b.iter(|| {
            let mut codec = Codec::new(&schema);
            let mut buf = [0u8; 64];
            for msg in &msgs {
                let _ = codec.encode(msg, &mut buf);
            }
        });
    });
}

fn bench_decode(c: &mut Criterion) {
    let schema = build_schema();
    let msgs = make_messages(10_000);

    // Pre-encode
    let mut enc = Codec::new(&schema);
    let encoded: Vec<Vec<u8>> = msgs
        .iter()
        .map(|msg| {
            let mut buf = [0u8; 64];
            let len = enc.encode(msg, &mut buf).unwrap();
            buf[..len].to_vec()
        })
        .collect();

    c.bench_function("decode", |b| {
        b.iter(|| {
            let mut codec = Codec::new(&schema);
            for frame in &encoded {
                let _ = codec.decode(frame);
            }
        });
    });
}

criterion_group!(benches, bench_encode, bench_decode);
criterion_main!(benches);
