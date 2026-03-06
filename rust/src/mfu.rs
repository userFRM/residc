use crate::MFU_SIZE;

/// Entry in the Most-Frequently-Used instrument table.
#[derive(Clone, Copy)]
pub struct MfuEntry {
    pub id: u16,
    pub count: u32,
}

/// Most-Frequently-Used instrument table.
///
/// Tracks the top-N instruments by frequency. Used to encode instrument IDs
/// with a compact index instead of a full 14-bit literal.
#[derive(Clone)]
pub struct MfuTable {
    pub entries: [MfuEntry; MFU_SIZE],
    len: usize,
}

impl MfuTable {
    pub fn new() -> Self {
        Self {
            entries: [MfuEntry { id: 0, count: 0 }; MFU_SIZE],
            len: 0,
        }
    }

    /// Look up an instrument ID. Returns its index if present in the table.
    pub fn lookup(&self, id: u16) -> Option<usize> {
        for i in 0..self.len {
            if self.entries[i].id == id {
                return Some(i);
            }
        }
        None
    }

    /// Record a use of `id`. If already in the table, increment its count
    /// and bubble it up. If not, insert it (replacing the least-used entry
    /// if the table is full).
    pub fn update(&mut self, id: u16) {
        // Check if already present
        for i in 0..self.len {
            if self.entries[i].id == id {
                self.entries[i].count += 1;
                // Bubble up while count exceeds predecessor
                let mut pos = i;
                while pos > 0 && self.entries[pos].count > self.entries[pos - 1].count {
                    self.entries.swap(pos, pos - 1);
                    pos -= 1;
                }
                return;
            }
        }

        // Not found — insert
        if self.len < MFU_SIZE {
            self.entries[self.len] = MfuEntry { id, count: 1 };
            self.len += 1;
        } else {
            // Replace the last (least frequent) entry
            let last = MFU_SIZE - 1;
            self.entries[last] = MfuEntry { id, count: 1 };
        }
    }

    /// Halve all counts (periodic decay to adapt to changing instrument mix).
    pub fn decay(&mut self) {
        for i in 0..self.len {
            self.entries[i].count >>= 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn insert_and_lookup() {
        let mut t = MfuTable::new();
        t.update(42);
        t.update(99);
        assert_eq!(t.lookup(42), Some(0));
        assert_eq!(t.lookup(99), Some(1));
        assert_eq!(t.lookup(7), None);
    }

    #[test]
    fn frequency_ordering() {
        let mut t = MfuTable::new();
        t.update(1);
        t.update(2);
        t.update(2);
        t.update(2);
        // ID 2 should bubble to index 0
        assert_eq!(t.entries[0].id, 2);
        assert_eq!(t.entries[1].id, 1);
    }

    #[test]
    fn decay_halves() {
        let mut t = MfuTable::new();
        for _ in 0..100 {
            t.update(5);
        }
        assert!(t.entries[0].count >= 100);
        t.decay();
        assert_eq!(t.entries[0].count, 50);
    }
}
