use residc::{Codec, FieldType, Message, Schema};

const N_MSGS: usize = 100_000;
const N_ITERS: usize = 10;

fn main() {
    let schema = Schema::builder()
        .field("timestamp", FieldType::Timestamp)
        .field("instrument", FieldType::Instrument)
        .field("price", FieldType::Price)
        .field("quantity", FieldType::Quantity)
        .field("side", FieldType::Bool)
        .build();

    // Generate messages
    let mut ts = 34_200_000_000_000u64;
    let msgs: Vec<Message> = (0..N_MSGS as u64)
        .map(|i| {
            ts += 1000 + (i * 37 % 50000);
            Message::new()
                .set(0, ts)
                .set(1, (i % 50) as u64)
                .set(2, (1_500_000 + (i * 7 % 2000)) as u64)
                .set(3, ((1 + i % 20) * 100) as u64)
                .set(4, (i % 2) as u64)
        })
        .collect();

    // Pre-encode for decode benchmark
    let mut enc = Codec::new(&schema);
    let mut total_compressed = 0usize;
    let encoded: Vec<Vec<u8>> = msgs
        .iter()
        .map(|msg| {
            let mut buf = [0u8; 64];
            let len = enc.encode(msg, &mut buf).unwrap();
            total_compressed += len;
            buf[..len].to_vec()
        })
        .collect();

    // Encode benchmark
    let mut best_enc = f64::MAX;
    for _ in 0..N_ITERS {
        let mut codec = Codec::new(&schema);
        let mut buf = [0u8; 64];
        let t0 = std::time::Instant::now();
        for msg in &msgs {
            let _ = codec.encode(msg, &mut buf);
        }
        let elapsed = t0.elapsed().as_nanos() as f64;
        let ns_per_msg = elapsed / N_MSGS as f64;
        if ns_per_msg < best_enc {
            best_enc = ns_per_msg;
        }
    }

    // Decode benchmark
    let mut best_dec = f64::MAX;
    for _ in 0..N_ITERS {
        let mut codec = Codec::new(&schema);
        let t0 = std::time::Instant::now();
        for frame in &encoded {
            let _ = codec.decode(frame);
        }
        let elapsed = t0.elapsed().as_nanos() as f64;
        let ns_per_msg = elapsed / N_MSGS as f64;
        if ns_per_msg < best_dec {
            best_dec = ns_per_msg;
        }
    }

    // Verify roundtrip
    let mut enc = Codec::new(&schema);
    let mut dec = Codec::new(&schema);
    let mut errors = 0;
    for msg in &msgs {
        let mut buf = [0u8; 64];
        let len = enc.encode(msg, &mut buf).unwrap();
        let decoded = dec.decode(&buf[..len]).unwrap();
        for fi in 0..5 {
            if decoded.get(fi) != msg.get(fi) {
                errors += 1;
                break;
            }
        }
    }

    let raw_size = schema.raw_size();
    println!(
        "residc Rust benchmark (5-field quote, {} messages, best of {} iterations)",
        N_MSGS, N_ITERS
    );
    println!("=================================================================");
    println!("  Encode:      {:.0} ns/msg", best_enc);
    println!("  Decode:      {:.0} ns/msg", best_dec);
    println!(
        "  Ratio:       {:.2}:1 ({} -> {:.1} bytes avg)",
        (N_MSGS * raw_size) as f64 / total_compressed as f64,
        raw_size,
        total_compressed as f64 / N_MSGS as f64
    );
    println!("  Errors:      {}", errors);
}
