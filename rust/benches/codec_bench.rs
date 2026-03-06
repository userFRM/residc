use criterion::{black_box, criterion_group, criterion_main, Criterion};
use residc::{Codec, FieldType};

fn quote_fields() -> Vec<FieldType> {
    vec![
        FieldType::Timestamp,
        FieldType::Instrument,
        FieldType::Price,
        FieldType::Quantity,
        FieldType::Enum,
    ]
}

fn bench_encode(c: &mut Criterion) {
    let fields = quote_fields();
    let mut codec = Codec::new(&fields, None).unwrap();
    let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];

    // Warm up state
    for i in 0..100u64 {
        let v = [
            34_200_000_000_000 + i * 1_000_000,
            42 + (i % 5),
            1_500_250 + (i % 100),
            100 + (i % 10),
            i % 3,
        ];
        let _ = codec.encode(&v);
    }

    c.bench_function("encode_quote", |b| {
        b.iter(|| {
            black_box(codec.encode(black_box(&values)).unwrap());
        })
    });
}

fn bench_decode(c: &mut Criterion) {
    let fields = quote_fields();
    let mut enc = Codec::new(&fields, None).unwrap();
    let mut dec = Codec::new(&fields, None).unwrap();

    // Warm up state
    for i in 0..100u64 {
        let v = [
            34_200_000_000_000 + i * 1_000_000,
            42 + (i % 5),
            1_500_250 + (i % 100),
            100 + (i % 10),
            i % 3,
        ];
        let compressed = enc.encode(&v).unwrap();
        dec.decode(&compressed).unwrap();
    }

    let values = [34_200_000_000_000u64, 42, 1_500_250, 100, 0];
    let compressed = enc.encode(&values).unwrap();

    c.bench_function("decode_quote", |b| {
        b.iter(|| {
            black_box(dec.decode(black_box(&compressed)).unwrap());
        })
    });
}

criterion_group!(benches, bench_encode, bench_decode);
criterion_main!(benches);
