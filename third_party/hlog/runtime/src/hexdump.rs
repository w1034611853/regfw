//! Bounded, row-at-a-time formatting. Numeric groups use native byte order,
//! without aligned loads; partial groups expose missing bytes as `??`.
use std::fmt::Write;

#[derive(Clone, Copy, Debug)]
pub(crate) struct HexdumpFormat(usize);

impl HexdumpFormat {
    pub(crate) fn from_ffi(bytes: u32) -> Result<Self, String> {
        match bytes {
            1 | 2 | 4 | 8 => Ok(Self(bytes as usize)),
            _ => Err("hexdump format must be 1, 2, 4, or 8 bytes".to_string()),
        }
    }
}

pub(crate) struct HexdumpRows<'a> {
    data: &'a [u8],
    format: HexdumpFormat,
    offset: usize,
    offset_width: usize,
    empty: bool,
}

impl<'a> HexdumpRows<'a> {
    pub(crate) fn new(data: &'a [u8], format: HexdumpFormat) -> Self {
        let last_offset = data.len().saturating_sub(1) / 16 * 16;
        Self {
            data,
            format,
            offset: 0,
            offset_width: format!("{last_offset:x}").len().max(8),
            empty: data.is_empty(),
        }
    }
}

impl Iterator for HexdumpRows<'_> {
    type Item = String;

    fn next(&mut self) -> Option<String> {
        if self.empty {
            self.empty = false;
            return Some("<empty>".to_string());
        }
        if self.offset >= self.data.len() {
            return None;
        }
        let chunk = &self.data[self.offset..self.data.len().min(self.offset.saturating_add(16))];
        let mut row = String::with_capacity(self.offset_width + 70);
        let _ = write!(row, "{:0width$x}  ", self.offset, width = self.offset_width);
        let hex_start = row.len();
        let unit = self.format.0;
        for start in (0..16).step_by(unit) {
            if start >= chunk.len() {
                for _ in 0..unit * 2 { row.push(' '); }
            } else {
                for position in 0..unit {
                    let byte = if cfg!(target_endian = "little") { unit - 1 - position } else { position };
                    if let Some(value) = chunk.get(start + byte) {
                        let _ = write!(row, "{value:02x}");
                    } else {
                        row.push_str("??");
                    }
                }
            }
            row.push(' ');
            if start + unit == 8 { row.push(' '); }
        }
        // Keep the ASCII column at the same position for every unit width.
        for _ in row.len() - hex_start..49 { row.push(' '); }
        row.push_str(" |");
        for &byte in chunk {
            row.push(if byte.is_ascii_graphic() || byte == b' ' { byte as char } else { '.' });
        }
        for _ in chunk.len()..16 { row.push(' '); }
        row.push('|');
        self.offset += chunk.len();
        Some(row)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows(data: &[u8], unit: u32) -> Vec<String> {
        HexdumpRows::new(data, HexdumpFormat::from_ffi(unit).unwrap()).collect()
    }

    #[test]
    fn byte_view_preserves_columns_offsets_and_ascii() {
        let data: Vec<_> = (0..20).collect();
        let dump = rows(&data, 1);
        assert_eq!(dump.len(), 2);
        assert!(dump[0].starts_with("00000000  00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f"));
        assert!(dump[1].starts_with("00000010  10 11 12 13"));
        assert_eq!(dump[0].find('|'), dump[1].find('|'));
        assert_eq!(dump[0].len(), dump[1].len());
        assert!(rows(b"ABC xyz", 1)[0].ends_with("|ABC xyz         |"));
    }

    #[test]
    fn numeric_groups_match_native_values_without_aligned_reads() {
        let data: Vec<_> = (0..33).collect();
        let unaligned = &data[1..];
        for unit in [1, 2, 4, 8] {
            let dump = rows(unaligned, unit);
            let value = match unit {
                1 => unaligned[0] as u64,
                2 => u16::from_ne_bytes(unaligned[..2].try_into().unwrap()) as u64,
                4 => u32::from_ne_bytes(unaligned[..4].try_into().unwrap()) as u64,
                _ => u64::from_ne_bytes(unaligned[..8].try_into().unwrap()),
            };
            assert!(dump[0].starts_with(&format!("00000000  {value:0width$x}", width = unit as usize * 2)));
            assert_eq!(dump[0].find('|'), Some(60));
            assert_eq!(dump[1].find('|'), Some(60));
            assert!(dump.iter().all(|line| !line.contains('\n')));
        }
    }

    #[test]
    fn partial_groups_never_invent_zero_bytes_or_drop_the_tail() {
        for unit in [2, 4, 8] {
            for tail in 1..unit {
                let data = vec![0xab; 16 + tail as usize];
                let dump = rows(&data, unit);
                assert_eq!(dump.len(), 2);
                assert_eq!(dump[1].matches("ab").count(), tail as usize);
                assert_eq!(dump[1].matches("??").count(), (unit - tail) as usize);
                assert_eq!(dump[0].find('|'), dump[1].find('|'));
            }
        }
        let partial = rows(&[0x12, 0x34, 0x56], 4);
        assert!(partial[0].contains(if cfg!(target_endian = "little") { "??563412" } else { "123456??" }));
    }

    #[test]
    fn empty_input_and_invalid_formats_are_explicit() {
        for unit in [1, 2, 4, 8] { assert_eq!(rows(&[], unit), ["<empty>"]); }
        for unit in [0, 3, 16, u32::MAX] { assert!(HexdumpFormat::from_ffi(unit).is_err()); }
    }
}
