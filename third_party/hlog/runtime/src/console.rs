//! Console-only writes and process-wide mirroring. Neither changes file levels.
use super::*;
use std::io::IsTerminal;

fn level_color(level: Level) -> &'static str {
    match level {
        Level::Trace => "\x1b[90m",
        Level::Debug => "\x1b[36m",
        Level::Info => "\x1b[32m",
        Level::Warn => "\x1b[33m",
        Level::Error => "\x1b[31m",
        Level::Critical => "\x1b[1;35m",
    }
}

#[derive(Clone)]
struct HLogConsoleFormatter;

impl Formatter for HLogConsoleFormatter {
    fn format(
        &self,
        record: &Record<'_>,
        dest: &mut StringBuf,
        ctx: &mut FormatterContext<'_>,
    ) -> spdlog::Result<()> {
        let color = match std::env::var("HLOG_CONSOLE_COLOR").as_deref() {
            Ok("always") => true,
            Ok("never") => false,
            _ => std::io::stderr().is_terminal() && std::env::var_os("NO_COLOR").is_none(),
        };
        if color {
            dest.write_str(level_color(record.level())).map_err(spdlog::Error::FormatRecord)?;
        }
        PatternFormatter::new(pattern!(
            "[HLOG] [{time}.{millisecond}] [{level}] [{logger}] [tid={tid}] {payload}{eol}"
        ))
        .format(record, dest, ctx)?;
        if color {
            dest.write_str("\x1b[0m").map_err(spdlog::Error::FormatRecord)?;
        }
        Ok(())
    }
}

pub(super) struct ConsoleOutput {
    pub(super) mirror: Arc<StdStreamSink>,
    direct: Arc<StdStreamSink>,
}

static CONSOLE_OUTPUT: OnceLock<ConsoleOutput> = OnceLock::new();

pub(super) fn console_output() -> Result<&'static ConsoleOutput, String> {
    if let Some(output) = CONSOLE_OUTPUT.get() {
        return Ok(output);
    }
    let build = |filter| {
        StdStreamSink::builder()
            .stderr()
            // Palette is explicit in HLogConsoleFormatter, not library defaults.
            .style_mode(StyleMode::Never)
            .level_filter(filter)
            .formatter(HLogConsoleFormatter)
            .build_arc()
            .map_err(|error| format!("failed to build console sink: {error}"))
    };
    let output = ConsoleOutput {
        mirror: build(LevelFilter::Off)?,
        direct: build(LevelFilter::All)?,
    };
    // Concurrent initializers may lose this race; only the published pair is
    // ever attached to loggers. No setter can be overwritten by a loser.
    let _ = CONSOLE_OUTPUT.set(output);
    Ok(CONSOLE_OUTPUT.get().expect("console was initialized"))
}

#[no_mangle]
pub extern "C" fn hlog_set_console_enabled(enabled: bool) -> i32 {
    ffi_guard_status(|| {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let _write = center.write_lock.lock().unwrap();
        let output = console_output().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        output.mirror.set_level_filter(if enabled { LevelFilter::All } else { LevelFilter::Off });
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_console_enabled() -> bool {
    CONSOLE_OUTPUT.get().is_some_and(|output| output.mirror.should_log(Level::Info))
}

fn write_console(entry: &LoggerEntry, level: LogLevel, function: &str, message: &str)
    -> Result<(), (i32, String)>
{
    with_console_emitter(entry, level, |inner| LoggerEntry::emit(inner, level, function, message))
}

fn with_console_emitter(entry: &LoggerEntry, level: LogLevel, emit: impl FnOnce(&Arc<Logger>))
    -> Result<(), (i32, String)>
{
    if level == LogLevel::Off {
        return Ok(());
    }
    let output = console_output().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
    // This private emitter is not registered in the public logger registry.
    // Always use the current name, including after logger_config_name().
    let inner = Logger::builder()
        .name(entry.display_name())
        .level_filter(LevelFilter::All)
        .sink(output.direct.clone())
        .build_arc()
        .map_err(|e| (HLOG_ERR_INTERNAL, format!("failed to build console emitter: {e}")))?;
    // Serialize complete records with mirrored writes and switch changes.
    let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
    let _write = center.write_lock.lock().unwrap();
    emit(&inner);
    Ok(())
}

#[no_mangle]
pub extern "C" fn hlog_console_write(
    handle: *const HLogLogger, level: u32,
    function: *const u8, function_len: usize,
    _file: *const u8, _file_len: usize, _line: u32,
    message: *const u8, message_len: usize,
) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let function = unsafe { bytes_arg(function, function_len, "function")? };
        let message = unsafe { bytes_arg(message, message_len, "message")? };
        write_console(entry, level, &String::from_utf8_lossy(function), &String::from_utf8_lossy(message))
    })
}

#[no_mangle]
pub extern "C" fn hlog_console_hexdump(
    handle: *const HLogLogger, level: u32,
    function: *const u8, function_len: usize,
    _file: *const u8, _file_len: usize, _line: u32,
    data: *const u8, data_len: usize,
) -> i32 {
    hlog_console_hexdump_ex(handle, level, function, function_len, _file, _file_len, _line, data, data_len, 1)
}

#[no_mangle]
pub extern "C" fn hlog_console_hexdump_ex(
    handle: *const HLogLogger, level: u32,
    function: *const u8, function_len: usize,
    _file: *const u8, _file_len: usize, _line: u32,
    data: *const u8, data_len: usize, unit_bytes: u32,
) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        if level == LogLevel::Off { return Ok(()); }
        let format = HexdumpFormat::from_ffi(unit_bytes).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let function = unsafe { bytes_arg(function, function_len, "function")? };
        let data = unsafe { bytes_arg(data, data_len, "data")? };
        let function = String::from_utf8_lossy(function);
        with_console_emitter(entry, level, |inner| {
            for row in HexdumpRows::new(data, format) {
                LoggerEntry::emit(inner, level, &function, &row);
            }
        })
    })
}
