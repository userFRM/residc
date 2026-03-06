use crate::MFU_SIZE;

const HASH_SIZE: usize = 512;
const HASH_EMPTY: u16 = 0xFFFF;

/// Entry in the Most-Frequently-Used instrument table.
#[derive(Clone, Copy)]
pub struct MfuEntry {
    pub id: u16,
    pub count: u32,
}

/// Most-Frequently-Used instrument table with hash acceleration.
///
/// Tracks the top-N instruments by frequency. Uses a 512-entry hash table
/// for O(1) lookup (matching the C implementation's hash+chain approach).
#[derive(Clone)]
pub struct MfuTable {
    pub entries: [MfuEntry; MFU_SIZE],
    len: usize,
    /// hash[id & (HASH_SIZE-1)] = index into entries[], or HASH_EMPTY if no mapping
    hash: [u16; HASH_SIZE],
}

impl MfuTable {
    pub fn new() -> Self {
        Self {
            entries: [MfuEntry { id: 0, count: 0 }; MFU_SIZE],
            len: 0,
            hash: [HASH_EMPTY; HASH_SIZE],
        }
    }

    #[inline(always)]
    fn hash_of(id: u16) -> usize {
        (id as usize) & (HASH_SIZE - 1)
    }

    /// Look up an instrument ID. Returns its index if present in the table.
    #[inline(always)]
    pub fn lookup(&self, id: u16) -> Option<usize> {
        let h = Self::hash_of(id);
        let idx = self.hash[h];
        if idx != HASH_EMPTY {
            let i = idx as usize;
            if self.entries[i].id == id {
                return Some(i);
            }
        }
        // Hash miss or collision — linear scan fallback
        self.entries[..self.len].iter().position(|e| e.id == id)
    }

    /// Record a use of `id`. If already in the table, increment its count
    /// and bubble it up. If not, insert it (replacing the least-used entry
    /// if the table is full).
    #[inline(always)]
    pub fn update(&mut self, id: u16) {
        // Check if already present (hash-accelerated)
        let mut found = None;
        let h = Self::hash_of(id);
        let hashed_idx = self.hash[h];
        if hashed_idx != HASH_EMPTY && self.entries[hashed_idx as usize].id == id {
            found = Some(hashed_idx as usize);
        } else {
            for i in 0..self.len {
                if self.entries[i].id == id {
                    found = Some(i);
                    break;
                }
            }
        }

        if let Some(i) = found {
            self.entries[i].count += 1;
            // Bubble up while count exceeds predecessor
            let mut pos = i;
            while pos > 0 && self.entries[pos].count > self.entries[pos - 1].count {
                // Update hash for both swapped entries
                let id_a = self.entries[pos].id;
                let id_b = self.entries[pos - 1].id;
                self.entries.swap(pos, pos - 1);
                self.hash[Self::hash_of(id_a)] = (pos - 1) as u16;
                self.hash[Self::hash_of(id_b)] = pos as u16;
                pos -= 1;
            }
            return;
        }

        // Not found — insert
        if self.len < MFU_SIZE {
            let idx = self.len;
            self.entries[idx] = MfuEntry { id, count: 1 };
            self.hash[h] = idx as u16;
            self.len += 1;
        } else {
            // Replace the last (least frequent) entry
            let last = MFU_SIZE - 1;
            // Clear hash for old entry
            let old_id = self.entries[last].id;
            let old_h = Self::hash_of(old_id);
            if self.hash[old_h] == last as u16 {
                self.hash[old_h] = HASH_EMPTY;
            }
            self.entries[last] = MfuEntry { id, count: 1 };
            self.hash[h] = last as u16;
        }
    }

    /// Pre-seed the table with known instruments.
    /// `instruments` is a slice of (id, count) pairs, sorted by descending frequency.
    /// Must be called identically on encoder and decoder.
    pub fn seed(&mut self, instruments: &[(u16, u32)]) {
        *self = Self::new();
        let n = instruments.len().min(MFU_SIZE);
        for (i, &(id, count)) in instruments[..n].iter().enumerate() {
            self.entries[i] = MfuEntry { id, count };
            let h = Self::hash_of(id);
            self.hash[h] = i as u16;
        }
        self.len = n;
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

    #[test]
    fn hash_accelerated_lookup() {
        let mut t = MfuTable::new();
        // Fill with 50 instruments (like the benchmark)
        for i in 0..50u16 {
            t.update(i);
        }
        // All should be found
        for i in 0..50u16 {
            assert!(t.lookup(i).is_some(), "instrument {} not found", i);
        }
        // Non-existent should return None
        assert_eq!(t.lookup(999), None);
    }
}
