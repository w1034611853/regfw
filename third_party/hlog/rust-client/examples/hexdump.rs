#[hlog::named]
fn main() -> hlog::LogResult<()> {
    let logger = hlog::create_module_logger("rust-buffer", hlog::LogLevel::Debug)?;
    hlog::set_console_enabled(true)?;
    let data: Vec<u8> = (0..35).collect();
    hlog::lhexdump_debug!(logger, hlog::HexdumpFormat::U8, data);
    hlog::lhexdump_debug!(logger, hlog::HexdumpFormat::U16, data);
    hlog::lhexdump_debug!(logger, hlog::HexdumpFormat::U32, &data[1..]);
    hlog::lhexdump_debug!(logger, hlog::HexdumpFormat::U64, data);
    hlog::lconsole_hexdump_info!(logger, hlog::HexdumpFormat::U32, data);
    hlog::flush_all();
    Ok(())
}
