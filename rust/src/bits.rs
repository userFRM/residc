/// Bit-level writer for building compressed frames.
pub struct BitWriter {
    pub buf: [u8; 256],
    byte_pos: usize,
    bit_pos: u32, // bits used in current byte (0..8)
}

impl BitWriter {
    pub fn new() -> Self {
        Self {
            buf: [0u8; 256],
            byte_pos: 0,
            bit_pos: 0,
        }
    }

    /// Write `num_bits` least-significant bits of `value`.
    #[inline]
    pub fn write(&mut self, value: u64, num_bits: u32) {
        let mut remaining = num_bits;
        let mut val = value;

        while remaining > 0 {
            let space = 8 - self.bit_pos;
            let chunk = remaining.min(space);
            let shift = remaining - chunk;
            let bits = ((val >> shift) & ((1u64 << chunk) - 1)) as u8;

            self.buf[self.byte_pos] |= bits << (space - chunk);
            self.bit_pos += chunk;
            remaining -= chunk;
            val &= (1u64 << shift) - 1;

            if self.bit_pos == 8 {
                self.byte_pos += 1;
                self.bit_pos = 0;
                if self.byte_pos < 256 {
                    self.buf[self.byte_pos] = 0;
                }
            }
        }
    }

    /// Finalize and return the number of bytes used.
    pub fn finish(&self) -> usize {
        if self.bit_pos > 0 {
            self.byte_pos + 1
        } else {
            self.byte_pos
        }
    }
}

/// Bit-level reader for decoding compressed frames.
pub struct BitReader<'a> {
    data: &'a [u8],
    byte_pos: usize,
    bit_pos: u32,
}

impl<'a> BitReader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, byte_pos: 0, bit_pos: 0 }
    }

    /// Read a single bit.
    #[inline]
    pub fn read_bit(&mut self) -> u64 {
        if self.byte_pos >= self.data.len() {
            return 0;
        }
        let bit = ((self.data[self.byte_pos] >> (7 - self.bit_pos)) & 1) as u64;
        self.bit_pos += 1;
        if self.bit_pos == 8 {
            self.byte_pos += 1;
            self.bit_pos = 0;
        }
        bit
    }

    /// Read `num_bits` and return as u64.
    #[inline]
    pub fn read(&mut self, num_bits: u32) -> u64 {
        let mut result = 0u64;
        for _ in 0..num_bits {
            result = (result << 1) | self.read_bit();
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_bits() {
        let mut bw = BitWriter::new();
        bw.write(0b101, 3);
        bw.write(0b1100, 4);
        bw.write(0xFF, 8);
        bw.write(1, 1);
        let len = bw.finish();

        let mut br = BitReader::new(&bw.buf[..len]);
        assert_eq!(br.read(3), 0b101);
        assert_eq!(br.read(4), 0b1100);
        assert_eq!(br.read(8), 0xFF);
        assert_eq!(br.read(1), 1);
    }

    #[test]
    fn single_bits() {
        let mut bw = BitWriter::new();
        bw.write(1, 1);
        bw.write(0, 1);
        bw.write(1, 1);
        let len = bw.finish();

        let mut br = BitReader::new(&bw.buf[..len]);
        assert_eq!(br.read_bit(), 1);
        assert_eq!(br.read_bit(), 0);
        assert_eq!(br.read_bit(), 1);
    }
}
