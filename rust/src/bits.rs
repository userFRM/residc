/// Bit-level writer that writes directly to an external buffer.
/// No internal buffer, no zeroing, no copy.
pub struct BitWriter<'a> {
    buf: &'a mut [u8],
    accum: u64,
    bits_in_accum: u32,
    byte_pos: usize,
}

impl<'a> BitWriter<'a> {
    #[inline]
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, accum: 0, bits_in_accum: 0, byte_pos: 0 }
    }

    /// Write bits to the accumulator. Fast path for <= 57 bits (covers all
    /// residual tiers and field encodings). Slow path splits for 64-bit escape.
    #[inline(always)]
    pub fn write(&mut self, value: u64, num_bits: u32) {
        debug_assert!(num_bits <= 64);

        if num_bits + self.bits_in_accum <= 64 {
            // Fast path: fits in accumulator.
            // num_bits < 64 here because the 64-bit escape tier always has
            // bits_in_accum > 0 (from the 5-bit prefix), forcing the slow path.
            let masked = value & ((1u64 << num_bits) - 1);
            self.accum = (self.accum << num_bits) | masked;
            self.bits_in_accum += num_bits;
            // Flush complete bytes
            while self.bits_in_accum >= 8 {
                self.bits_in_accum -= 8;
                unsafe {
                    *self.buf.as_mut_ptr().add(self.byte_pos) =
                        (self.accum >> self.bits_in_accum) as u8;
                }
                self.byte_pos += 1;
            }
        } else {
            // Slow path: split write (only for 64-bit escape tier)
            let first = 64 - self.bits_in_accum;
            let second = num_bits - first;
            self.write(value >> second, first);
            self.write(value & ((1u64 << second) - 1), second);
        }
    }

    /// Finalize: flush remaining bits and return total bytes written.
    /// Consumes self to release the buffer borrow.
    #[inline]
    pub fn finish(mut self) -> usize {
        if self.bits_in_accum > 0 {
            unsafe {
                *self.buf.as_mut_ptr().add(self.byte_pos) =
                    (self.accum << (8 - self.bits_in_accum)) as u8;
            }
            self.byte_pos += 1;
        }
        self.byte_pos
    }
}

/// Bit-level reader with pointer-based access, no bounds checks on hot path.
pub struct BitReader<'a> {
    data: &'a [u8],
    accum: u64,
    bits_in_accum: u32,
    byte_pos: usize,
}

impl<'a> BitReader<'a> {
    #[inline]
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, accum: 0, bits_in_accum: 0, byte_pos: 0 }
    }

    /// Refill accumulator until we have at least `need` bits.
    #[inline(always)]
    fn refill(&mut self, need: u32) {
        while self.bits_in_accum < need && self.byte_pos < self.data.len() {
            let byte = unsafe { *self.data.as_ptr().add(self.byte_pos) };
            self.accum = (self.accum << 8) | byte as u64;
            self.bits_in_accum += 8;
            self.byte_pos += 1;
        }
    }

    /// Read a single bit.
    #[inline(always)]
    pub fn read_bit(&mut self) -> u64 {
        self.refill(1);
        if self.bits_in_accum == 0 { return 0; }
        self.bits_in_accum -= 1;
        (self.accum >> self.bits_in_accum) & 1
    }

    /// Read `num_bits` and return as u64.
    #[inline(always)]
    pub fn read(&mut self, num_bits: u32) -> u64 {
        debug_assert!(num_bits > 0 && num_bits <= 64);
        self.refill(num_bits);
        if self.bits_in_accum < num_bits {
            // Need more bits than available — split (64-bit escape only)
            let first = self.bits_in_accum;
            let hi = self.accum & ((1u64 << first) - 1);
            self.bits_in_accum = 0;
            self.accum = 0;
            self.refill(num_bits - first);
            let second = num_bits - first;
            let lo = self.read(second);
            (hi << second) | lo
        } else {
            self.bits_in_accum -= num_bits;
            let mask = if num_bits >= 64 { u64::MAX } else { (1u64 << num_bits) - 1 };
            (self.accum >> self.bits_in_accum) & mask
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_bits() {
        let mut buf = [0u8; 256];
        let len = {
            let mut bw = BitWriter::new(&mut buf);
            bw.write(0b101, 3);
            bw.write(0b1100, 4);
            bw.write(0xFF, 8);
            bw.write(1, 1);
            bw.finish()
        };

        let mut br = BitReader::new(&buf[..len]);
        assert_eq!(br.read(3), 0b101);
        assert_eq!(br.read(4), 0b1100);
        assert_eq!(br.read(8), 0xFF);
        assert_eq!(br.read(1), 1);
    }

    #[test]
    fn single_bits() {
        let mut buf = [0u8; 256];
        let len = {
            let mut bw = BitWriter::new(&mut buf);
            bw.write(1, 1);
            bw.write(0, 1);
            bw.write(1, 1);
            bw.finish()
        };

        let mut br = BitReader::new(&buf[..len]);
        assert_eq!(br.read_bit(), 1);
        assert_eq!(br.read_bit(), 0);
        assert_eq!(br.read_bit(), 1);
    }

    #[test]
    fn large_values() {
        let mut buf = [0u8; 256];
        let len = {
            let mut bw = BitWriter::new(&mut buf);
            bw.write(0xDEADBEEF, 32);
            bw.write(0x1234, 16);
            bw.finish()
        };

        let mut br = BitReader::new(&buf[..len]);
        assert_eq!(br.read(32), 0xDEADBEEF);
        assert_eq!(br.read(16), 0x1234);
    }

    #[test]
    fn mixed_small_large() {
        let mut buf = [0u8; 256];
        let len = {
            let mut bw = BitWriter::new(&mut buf);
            bw.write(1, 1);
            bw.write(0xABCD, 16);
            bw.write(0, 3);
            bw.write(0b11, 2);
            bw.finish()
        };

        let mut br = BitReader::new(&buf[..len]);
        assert_eq!(br.read_bit(), 1);
        assert_eq!(br.read(16), 0xABCD);
        assert_eq!(br.read(3), 0);
        assert_eq!(br.read(2), 0b11);
    }

    #[test]
    fn stress_many_small_writes() {
        let mut buf = [0u8; 256];
        let len = {
            let mut bw = BitWriter::new(&mut buf);
            for i in 0..100u64 {
                bw.write(i & 0x7, 3);
            }
            bw.finish()
        };

        let mut br = BitReader::new(&buf[..len]);
        for i in 0..100u64 {
            assert_eq!(br.read(3), i & 0x7, "mismatch at i={i}");
        }
    }
}
