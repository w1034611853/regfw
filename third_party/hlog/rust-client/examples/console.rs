use function_name::named;

#[named]
fn main() -> hlog::LogResult<()> {
    let logger = hlog::create_module_logger("rust-console", hlog::LogLevel::Off)?;
    hlog::lconsole_trace!(logger, "trace");
    hlog::lconsole_debug!(logger, "vf={}", 3);
    hlog::lconsole_info!(logger, "{:<8} {:>8}", "L_POOL", "VL_POOL");
    hlog::lconsole_warn!(logger, "address=0x{:x}", 0x1234);
    hlog::lconsole_error!(logger, "error");
    hlog::lconsole_critical!(logger, "critical");
    hlog::lconsole_hexdump!(logger, hlog::LogLevel::Debug, [0x12u8, 0x34, 0, 0xff]);
    Ok(())
}
