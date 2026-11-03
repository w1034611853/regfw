use std::cell::Cell;
use hlog::{HexdumpFormat as F, LogLevel};

#[test]
#[hlog::named]
fn macros_preserve_levels_formats_and_lazy_evaluation() {
    let logger = hlog::create_named_logger("rust-hexdump-test", "0", LogLevel::Trace).unwrap();
    let data: [u8; 35] = std::array::from_fn(|i| i as u8);
    let logger_reads = Cell::new(0);
    let level_reads = Cell::new(0);
    let format_reads = Cell::new(0);
    let buffer_reads = Cell::new(0);
    hlog::lhexdump!(
        { logger_reads.set(logger_reads.get()+1); logger.clone() },
        { level_reads.set(level_reads.get()+1); LogLevel::Debug },
        { format_reads.set(format_reads.get()+1); F::U32 },
        { buffer_reads.set(buffer_reads.get()+1); data }
    );
    assert_eq!((logger_reads.get(),level_reads.get(),format_reads.get(),buffer_reads.get()),(1,1,1,1));
    logger.disable();
    hlog::lhexdump_debug!(
        { logger_reads.set(logger_reads.get()+1); logger.clone() },
        { format_reads.set(format_reads.get()+1); F::U64 },
        { buffer_reads.set(buffer_reads.get()+1); data }
    );
    assert_eq!((logger_reads.get(),format_reads.get(),buffer_reads.get()),(2,1,1));
    hlog::lhexdump!(logger,{level_reads.set(level_reads.get()+1);LogLevel::Debug},data);
    assert_eq!(level_reads.get(),2);

    // Explicit console remains usable while the file logger is OFF.
    hlog::lconsole_hexdump_debug!(
        logger, {format_reads.set(format_reads.get()+1);F::U64},
        {buffer_reads.set(buffer_reads.get()+1);data}
    );
    assert_eq!((format_reads.get(),buffer_reads.get()),(2,2));

    logger.set_level(LogLevel::Trace);
    hlog::lhexdump_trace!(logger,F::U8,data);
    hlog::lhexdump_info!(logger,F::U16,data);
    hlog::lhexdump_warn!(logger,F::U32,data);
    hlog::lhexdump_error!(logger,F::U64,data);
    hlog::lhexdump_critical!(logger,F::U8,[]);
    hlog::lhexdump!(logger,LogLevel::Debug,data); // Existing byte API.
    hlog::lconsole_hexdump!(logger,LogLevel::Debug,data);
    hlog::lconsole_hexdump_trace!(logger,F::U8,[]);
    hlog::lconsole_hexdump_warn!(logger,F::U16,[]);
    hlog::lconsole_hexdump_error!(logger,F::U32,[]);
    hlog::lconsole_hexdump_critical!(logger,F::U64,[]);
    hlog::lhexdump_info!(F::U32,data);
    hlog::lconsole_hexdump_info!(F::U32,[]);
    logger.hexdump_format(LogLevel::Debug,F::U32,&data);
    logger.flush();
}
