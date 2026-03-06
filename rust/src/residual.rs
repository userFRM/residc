use crate::ADAPT_WINDOW;
use crate::bits::{BitReader, BitWriter};

/// Zigzag-encode a signed value to unsigned.
/// Maps 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ...
#[inline(always)]
pub fn zigzag_enc(v: i64) -> u64 {
    ((v << 1) ^ (v >> 63)) as u64
}

/// Zigzag-decode an unsigned value back to signed.
#[inline(always)]
pub fn zigzag_dec(v: u64) -> i64 {
    ((v >> 1) as i64) ^ -((v & 1) as i64)
}

/// Tiered residual encoding.
///
/// Five tiers with increasing capacity:
///   T0: 0           → 1 bit   (value 0)
///   T1: 10 + k bits → 2+k bits (small residual)
///   T2: 110 + 2k bits → 3+2k bits
///   T3: 1110 + 3k bits → 4+3k bits
///   T4: 11110 + 32 bits → 37 bits (escape tier)
///   T5: 11111 + 64 bits → 69 bits (full 64-bit escape)
#[inline(always)]
pub fn encode(bw: &mut BitWriter<'_>, value: i64, k: u32) {
    let zz = zigzag_enc(value);

    if zz == 0 {
        bw.write(0, 1);
        return;
    }

    let t1_max = (1u64 << k) - 1;
    let t2_max = t1_max + (1u64 << (2 * k));
    let t3_max = t2_max + (1u64 << (3 * k));

    if zz <= t1_max {
        // Combine prefix + payload into single write (max 2+20=22 bits)
        bw.write((0b10 << k) | (zz - 1), 2 + k);
    } else if zz <= t2_max {
        // Combine prefix + payload (max 3+40=43 bits)
        bw.write((0b110 << (2 * k)) | (zz - t1_max - 1), 3 + 2 * k);
    } else if zz <= t3_max {
        // Tier 3 can exceed 57 bits (4+3*20=64), keep as two writes
        bw.write(0b1110, 4);
        bw.write(zz - t2_max - 1, 3 * k);
    } else if zz <= u32::MAX as u64 {
        bw.write(0b11110, 5);
        bw.write(zz, 32);
    } else {
        bw.write(0b11111, 5);
        bw.write(zz, 64);
    }
}

/// Tiered residual decoding — mirror of encode.
#[inline(always)]
pub fn decode(br: &mut BitReader, k: u32) -> i64 {
    if br.read_bit() == 0 {
        return 0;
    }

    let t1_max = (1u64 << k) - 1;
    let t2_max = t1_max + (1u64 << (2 * k));

    let zz = if br.read_bit() == 0 {
        // T1
        br.read(k) + 1
    } else if br.read_bit() == 0 {
        // T2
        br.read(2 * k) + t1_max + 1
    } else if br.read_bit() == 0 {
        // T3
        br.read(3 * k) + t2_max + 1
    } else if br.read_bit() == 0 {
        // T4: 32-bit escape
        br.read(32)
    } else {
        // T5: 64-bit escape
        br.read(64)
    };

    zigzag_dec(zz)
}

/// Compute adaptive k based on recent residual magnitudes.
/// Returns a value in [k_min, k_max].
#[inline(always)]
pub fn adaptive_k(sum: u64, count: u32, k_min: u32, k_max: u32) -> u32 {
    if count == 0 {
        return k_min;
    }
    let avg = sum / count as u64;
    let bits = if avg == 0 {
        0
    } else {
        64 - avg.leading_zeros()
    };
    bits.clamp(k_min, k_max)
}

/// Update the running adaptive window (sum and count).
#[inline(always)]
pub fn adaptive_update(sum: &mut u64, count: &mut u32, zz: u64) {
    *sum += zz;
    *count += 1;
    if *count >= ADAPT_WINDOW {
        *sum >>= 1;
        *count >>= 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zigzag_roundtrip() {
        for v in [-1000, -1, 0, 1, 42, i64::MAX, i64::MIN] {
            assert_eq!(zigzag_dec(zigzag_enc(v)), v);
        }
    }

    #[test]
    fn residual_roundtrip() {
        for k in [2, 3, 5, 8] {
            for &v in &[0i64, 1, -1, 7, -7, 100, -100, 100_000, -100_000] {
                let mut buf = [0u8; 256];
                let len = {
                    let mut bw = BitWriter::new(&mut buf);
                    encode(&mut bw, v, k);
                    bw.finish()
                };
                let mut br = BitReader::new(&buf[..len]);
                let decoded = decode(&mut br, k);
                assert_eq!(decoded, v, "k={k}, value={v}");
            }
        }
    }

    #[test]
    fn adaptive_k_basic() {
        assert_eq!(adaptive_k(0, 0, 3, 10), 3);
        assert_eq!(adaptive_k(1000, 10, 3, 10), 7); // avg=100, bits=7
        assert_eq!(adaptive_k(1, 1, 5, 12), 5); // avg=1, bits=1, clamped to 5
    }
}
