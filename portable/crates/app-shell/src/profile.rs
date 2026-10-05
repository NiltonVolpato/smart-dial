// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

//! Statistical profiling data structures for tracking program counter (PC) hotspots.

/// A program counter sample and the number of times it was observed.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct PcSample {
    pub pc: u32,
    pub count: u32,
}

/// A fixed-size, open-addressing hash table for storing PC samples without heap allocation.
#[derive(Clone, Debug)]
pub struct SampleTable<const CAPACITY: usize = 128> {
    entries: [PcSample; CAPACITY],
    total_samples: u32,
    unique_count: usize,
}

impl<const CAPACITY: usize> Default for SampleTable<CAPACITY> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const CAPACITY: usize> SampleTable<CAPACITY> {
    pub const fn new() -> Self {
        Self {
            entries: [PcSample { pc: 0, count: 0 }; CAPACITY],
            total_samples: 0,
            unique_count: 0,
        }
    }

    /// Records a program counter observation.
    /// Returns `true` if recorded successfully, or `false` if the table is completely full.
    pub fn record(&mut self, pc: u32) -> bool {
        if pc == 0 {
            return false;
        }

        // Knuth's multiplicative hash for 32-bit values
        let mut index = ((pc >> 2).wrapping_mul(0x9E3779B9) as usize) % CAPACITY;

        for _ in 0..CAPACITY {
            if self.entries[index].pc == pc {
                self.entries[index].count = self.entries[index].count.saturating_add(1);
                self.total_samples = self.total_samples.saturating_add(1);
                return true;
            }
            if self.entries[index].pc == 0 {
                self.entries[index].pc = pc;
                self.entries[index].count = 1;
                self.total_samples = self.total_samples.saturating_add(1);
                self.unique_count += 1;
                return true;
            }
            index = (index + 1) % CAPACITY;
        }

        false
    }

    /// Returns the total number of samples recorded.
    pub const fn total_samples(&self) -> u32 {
        self.total_samples
    }

    /// Returns the number of unique PC addresses recorded.
    pub const fn unique_count(&self) -> usize {
        self.unique_count
    }

    /// Resets all entries and counters in the table.
    pub fn reset(&mut self) {
        self.entries = [PcSample { pc: 0, count: 0 }; CAPACITY];
        self.total_samples = 0;
        self.unique_count = 0;
    }

    /// Extracts the top `N` PC samples sorted in descending order of observation count.
    /// Returns the array of top samples and the number of valid items in it (`min(unique_count, N)`).
    pub fn top_samples<const N: usize>(&self) -> ([PcSample; N], usize) {
        let mut top = [PcSample { pc: 0, count: 0 }; N];
        let mut count = 0;

        for entry in self.entries.iter() {
            if entry.pc == 0 || entry.count == 0 {
                continue;
            }

            if count < N {
                top[count] = *entry;
                count += 1;
                // Keep the array sorted as we insert
                let mut j = count - 1;
                while j > 0 && top[j].count > top[j - 1].count {
                    top.swap(j, j - 1);
                    j -= 1;
                }
            } else if entry.count > top[N - 1].count {
                top[N - 1] = *entry;
                let mut j = N - 1;
                while j > 0 && top[j].count > top[j - 1].count {
                    top.swap(j, j - 1);
                    j -= 1;
                }
            }
        }

        (top, count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_record_single_sample() {
        let mut table = SampleTable::<16>::new();
        assert_eq!(table.total_samples(), 0);
        assert_eq!(table.unique_count(), 0);

        assert!(table.record(0x42001000));
        assert_eq!(table.total_samples(), 1);
        assert_eq!(table.unique_count(), 1);

        assert!(table.record(0x42001000));
        assert_eq!(table.total_samples(), 2);
        assert_eq!(table.unique_count(), 1);
    }

    #[test]
    fn test_ignore_null_pc() {
        let mut table = SampleTable::<16>::new();
        assert!(!table.record(0));
        assert_eq!(table.total_samples(), 0);
    }

    #[test]
    fn test_multiple_unique_samples() {
        let mut table = SampleTable::<16>::new();
        assert!(table.record(0x42001000));
        assert!(table.record(0x42002000));
        assert!(table.record(0x42003000));

        assert_eq!(table.total_samples(), 3);
        assert_eq!(table.unique_count(), 3);
    }

    #[test]
    fn test_top_samples_ordering() {
        let mut table = SampleTable::<16>::new();
        // 0x1000 -> 3 hits
        table.record(0x1000);
        table.record(0x1000);
        table.record(0x1000);

        // 0x2000 -> 5 hits
        for _ in 0..5 {
            table.record(0x2000);
        }

        // 0x3000 -> 1 hit
        table.record(0x3000);

        // 0x4000 -> 2 hits
        table.record(0x4000);
        table.record(0x4000);

        let (top, len) = table.top_samples::<3>();
        assert_eq!(len, 3);
        assert_eq!(top[0], PcSample { pc: 0x2000, count: 5 });
        assert_eq!(top[1], PcSample { pc: 0x1000, count: 3 });
        assert_eq!(top[2], PcSample { pc: 0x4000, count: 2 });
    }

    #[test]
    fn test_table_saturation() {
        let mut table = SampleTable::<4>::new();
        assert!(table.record(0x1000));
        assert!(table.record(0x2000));
        assert!(table.record(0x3000));
        assert!(table.record(0x4000));
        assert_eq!(table.unique_count(), 4);

        // Recording existing PC still works when full
        assert!(table.record(0x1000));
        assert_eq!(table.total_samples(), 5);

        // New PC cannot be inserted when full
        assert!(!table.record(0x5000));
        assert_eq!(table.total_samples(), 5);
        assert_eq!(table.unique_count(), 4);
    }

    #[test]
    fn test_reset() {
        let mut table = SampleTable::<16>::new();
        table.record(0x1000);
        table.record(0x2000);
        assert_eq!(table.total_samples(), 2);

        table.reset();
        assert_eq!(table.total_samples(), 0);
        assert_eq!(table.unique_count(), 0);

        let (_, len) = table.top_samples::<5>();
        assert_eq!(len, 0);
    }
}
