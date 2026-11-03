use std::{
    cell::RefCell,
    ffi::{c_void, CStr, CString},
    fmt,
    path::{Path, PathBuf},
    ptr::NonNull,
    sync::Arc,
    time::Duration,
};

pub use function_name::named;

pub type LogResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
pub type SharedLogger = Arc<ModuleLogger>;

/// Unsigned groups in native byte order; buffers and offsets remain byte-based.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum HexdumpFormat { U8 = 1, U16 = 2, U32 = 4, U64 = 8 }

const ABI_VERSION: u32 = 3;
const HLOG_OK: i32 = 0;
const HLOG_ERR_INVALID_ARGUMENT: i32 = -1;
const HLOG_ERR_INTERNAL: i32 = -2;
const HLOG_QUERY_PARAM_REQUIRED: u32 = 1 << 0;
const HLOG_QUERY_PARAM_REPEATABLE: u32 = 1 << 1;
const DEFAULT_LOGGER_NAME: &str = "__default__";
const DEFAULT_ROTATION_SIZE: u64 = 100 * 1024 * 1024;
const DEFAULT_MAX_FILES: u32 = 10;
const DEFAULT_FLUSH_INTERVAL: Duration = Duration::from_secs(2);

mod ffi {
    use std::{ffi::c_void, os::raw::c_char};

    #[repr(C)]
    pub struct HLogLogger {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct HLogConfig {
        pub struct_size: u32,
        pub abi_version: u32,
        pub base_dir: *const c_char,
        pub file_name: *const c_char,
        pub default_level: u32,
        pub truncate: u8,
        pub reserved: [u8; 7],
        pub rotation_size_bytes: u64,
        pub max_files: u32,
        pub flush_interval_ms: u32,
    }

    #[repr(C)]
    pub struct HLogQueryParamSpec {
        pub name: *const c_char,
        pub param_type: u32,
        pub flags: u32,
        pub description: *const c_char,
    }

    #[repr(C)]
    pub struct HLogQueryArg {
        pub name: *const c_char,
        pub value: *const c_char,
    }

    #[repr(C)]
    pub struct HLogQueryRequest {
        pub args: *const HLogQueryArg,
        pub arg_count: usize,
    }

    pub type HLogQueryWriteFn = unsafe extern "C" fn(*mut c_void, *const u8, usize) -> i32;
    pub type HLogQueryCallback = unsafe extern "C" fn(
        *mut c_void,
        *const HLogQueryRequest,
        HLogQueryWriteFn,
        *mut c_void,
    ) -> i32;

    #[link(name = "hlog")]
    extern "C" {
        pub fn hlog_abi_version() -> u32;
        pub fn hlog_last_error() -> *const c_char;
        pub fn hlog_init_default() -> i32;
        pub fn hlog_init(config: *const HLogConfig) -> i32;
        pub fn hlog_load_level_config(path: *const c_char) -> i32;
        pub fn hlog_set_level_rule(target: *const c_char, level: u32) -> i32;
        pub fn hlog_query_register(
            module: *const c_char,
            instance: *const c_char,
            name: *const c_char,
            description: *const c_char,
            params: *const HLogQueryParamSpec,
            param_count: usize,
            callback: Option<HLogQueryCallback>,
            opaque: *mut c_void,
        ) -> u64;
        pub fn hlog_query_unregister(handle: u64) -> i32;
        pub fn hlog_query_set_error(code: i32, message: *const c_char) -> i32;

        pub fn hlog_register_logger(
            module_name: *const c_char,
            configured_name: *const c_char,
            level: u32,
        ) -> *mut HLogLogger;

        pub fn hlog_default_logger() -> *mut HLogLogger;
        pub fn hlog_get_logger(module_name: *const c_char) -> *mut HLogLogger;
        pub fn hlog_module_logger_count(module_name: *const c_char) -> usize;
        pub fn hlog_module_logger_at(module_name: *const c_char, index: usize) -> *mut HLogLogger;

        pub fn hlog_module_count() -> usize;
        pub fn hlog_module_name_at(index: usize, buffer: *mut c_char, capacity: usize) -> usize;

        pub fn hlog_logger_module_name(
            logger: *const HLogLogger,
            buffer: *mut c_char,
            capacity: usize,
        ) -> usize;
        pub fn hlog_logger_configured_name(
            logger: *const HLogLogger,
            buffer: *mut c_char,
            capacity: usize,
        ) -> usize;
        pub fn hlog_logger_display_name(
            logger: *const HLogLogger,
            buffer: *mut c_char,
            capacity: usize,
        ) -> usize;
        pub fn hlog_logger_instance_id(logger: *const HLogLogger) -> usize;
        pub fn hlog_logger_level(logger: *const HLogLogger) -> u32;
        pub fn hlog_logger_config_name(logger: *mut HLogLogger, new_name: *const c_char) -> i32;

        pub fn hlog_enabled(logger: *const HLogLogger, level: u32) -> bool;
        pub fn hlog_set_console_enabled(enabled: bool) -> i32;
        pub fn hlog_console_enabled() -> bool;
        pub fn hlog_console_write(
            logger: *const HLogLogger, level: u32,
            function: *const u8, function_len: usize,
            file: *const u8, file_len: usize, line: u32,
            message: *const u8, message_len: usize,
        ) -> i32;
        pub fn hlog_console_hexdump(
            logger: *const HLogLogger, level: u32,
            function: *const u8, function_len: usize,
            file: *const u8, file_len: usize, line: u32,
            data: *const u8, data_len: usize,
        ) -> i32;
        pub fn hlog_write(
            logger: *const HLogLogger,
            level: u32,
            function: *const u8,
            function_len: usize,
            file: *const u8,
            file_len: usize,
            line: u32,
            message: *const u8,
            message_len: usize,
        ) -> i32;
        pub fn hlog_hexdump(
            logger: *const HLogLogger,
            level: u32,
            function: *const u8,
            function_len: usize,
            file: *const u8,
            file_len: usize,
            line: u32,
            data: *const u8,
            data_len: usize,
        ) -> i32;
        pub fn hlog_hexdump_ex(
            logger: *const HLogLogger, level: u32,
            function: *const u8, function_len: usize,
            file: *const u8, file_len: usize, line: u32,
            data: *const u8, data_len: usize, format: u32,
        ) -> i32;
        pub fn hlog_console_hexdump_ex(
            logger: *const HLogLogger, level: u32,
            function: *const u8, function_len: usize,
            file: *const u8, file_len: usize, line: u32,
            data: *const u8, data_len: usize, format: u32,
        ) -> i32;

        pub fn hlog_logger_set_level(logger: *mut HLogLogger, level: u32) -> i32;
        pub fn hlog_set_default_level(level: u32) -> i32;
        pub fn hlog_set_module_level(module_name: *const c_char, level: u32) -> i32;
        pub fn hlog_set_all_levels(level: u32) -> i32;

        pub fn hlog_flush() -> i32;
        pub fn hlog_set_flush_interval_ms(interval_ms: u32) -> i32;
        pub fn hlog_file_path(buffer: *mut c_char, capacity: usize) -> usize;
        pub fn hlog_shutdown();
    }
}


/// Enable/disable process-wide mirroring of accepted file logs to stderr.
pub fn set_console_enabled(enabled: bool) -> LogResult<()> {
    check_rc(unsafe { ffi::hlog_set_console_enabled(enabled) })
}

pub fn console_enabled() -> bool {
    unsafe { ffi::hlog_console_enabled() }
}

/// Explicit console-only trace message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_trace {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Trace, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

/// Explicit console-only debug message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_debug {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Debug, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

/// Explicit console-only info message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_info {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Info, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

/// Explicit console-only warn message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_warn {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Warn, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

/// Explicit console-only error message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_error {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Error, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

/// Explicit console-only critical message, unaffected by file levels.
#[macro_export]
macro_rules! lconsole_critical {
    ($logger:expr, $($arg:tt)*) => {{
        let hlog_logger_value = &$logger;
        hlog_logger_value.console_write_context(
            $crate::LogLevel::Critical, function_name!(), file!(), line!(),
            format_args!($($arg)*)
        );
    }};
}

#[macro_export]
macro_rules! lconsole_hexdump {
    ($logger:expr, $level:expr, $format:expr, $data:expr) => {{
        let hlog_logger_value = &$logger;
        let hlog_level_value = $level;
        if hlog_level_value != $crate::LogLevel::Off {
            let hlog_data_value = &$data;
            let hlog_bytes: &[u8] = hlog_data_value.as_ref();
            hlog_logger_value.hexdump_format_context(
                hlog_level_value, $format, function_name!(), file!(), line!(), hlog_bytes, true,
            );
        }
    }};
    ($logger:expr, $level:expr, $data:expr) => {{
        let hlog_logger_value = &$logger;
        let hlog_level_value = $level;
        if hlog_level_value != $crate::LogLevel::Off {
            let hlog_data_value = &$data;
            let hlog_bytes: &[u8] = hlog_data_value.as_ref();
            hlog_logger_value.console_hexdump_context(
                hlog_level_value, function_name!(), file!(), line!(), hlog_bytes,
            );
        }
    }};
}

thread_local! {
    // Reuse formatting capacity on hot logging paths. Re-entrant logging falls
    // back to a temporary String instead of panicking on RefCell borrowing.
    static FORMAT_BUFFER: RefCell<String> = RefCell::new(String::with_capacity(512));
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
}

impl LogLevel {
    fn from_raw(value: u32) -> Self {
        match value {
            0 => Self::Trace,
            1 => Self::Debug,
            2 => Self::Info,
            3 => Self::Warn,
            4 => Self::Error,
            5 => Self::Critical,
            _ => Self::Off,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum QueryParamType {
    String = 0,
    U32 = 1,
    U64 = 2,
    I64 = 3,
    Bool = 4,
}

#[derive(Debug, Clone)]
pub struct QueryParam {
    name: String,
    param_type: QueryParamType,
    required: bool,
    repeatable: bool,
    description: String,
}

impl QueryParam {
    fn new(name: impl Into<String>, param_type: QueryParamType, description: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            param_type,
            required: false,
            repeatable: false,
            description: description.into(),
        }
    }

    pub fn required(mut self) -> Self {
        self.required = true;
        self
    }

    pub fn repeatable(mut self) -> Self {
        self.repeatable = true;
        self
    }
}

#[derive(Debug, Clone)]
pub struct QueryError {
    code: i32,
    message: String,
}

impl QueryError {
    pub fn invalid_argument(message: impl Into<String>) -> Self {
        Self {
            code: HLOG_ERR_INVALID_ARGUMENT,
            message: message.into(),
        }
    }

    pub fn internal(message: impl Into<String>) -> Self {
        Self {
            code: HLOG_ERR_INTERNAL,
            message: message.into(),
        }
    }

    pub fn code(&self) -> i32 {
        self.code
    }
}

impl fmt::Display for QueryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for QueryError {}

pub type QueryResult<T> = Result<T, QueryError>;

pub struct QueryRequest<'a> {
    raw: &'a ffi::HLogQueryRequest,
}

impl<'a> QueryRequest<'a> {
    unsafe fn from_raw(raw: *const ffi::HLogQueryRequest) -> QueryResult<Self> {
        raw.as_ref()
            .map(|raw| Self { raw })
            .ok_or_else(|| QueryError::internal("hlog supplied a NULL query request"))
    }

    fn raw_args(&self) -> QueryResult<&'a [ffi::HLogQueryArg]> {
        if self.raw.arg_count == 0 {
            return Ok(&[]);
        }
        if self.raw.args.is_null() {
            return Err(QueryError::internal(
                "hlog supplied NULL query args with nonzero arg_count",
            ));
        }
        Ok(unsafe { std::slice::from_raw_parts(self.raw.args, self.raw.arg_count) })
    }

    pub fn values(&self, name: &str) -> QueryResult<Vec<&'a str>> {
        let mut values = Vec::new();
        for arg in self.raw_args()? {
            if arg.name.is_null() || arg.value.is_null() {
                return Err(QueryError::internal("hlog supplied a NULL query argument"));
            }
            let arg_name = unsafe { CStr::from_ptr(arg.name) }
                .to_str()
                .map_err(|error| QueryError::internal(format!("query argument name is not UTF-8: {error}")))?;
            if arg_name == name {
                let value = unsafe { CStr::from_ptr(arg.value) }
                    .to_str()
                    .map_err(|error| QueryError::internal(format!("query argument value is not UTF-8: {error}")))?;
                values.push(value);
            }
        }
        Ok(values)
    }

    pub fn optional_string(&self, name: &str) -> QueryResult<Option<&'a str>> {
        Ok(self.values(name)?.into_iter().next())
    }

    pub fn required_string(&self, name: &str) -> QueryResult<&'a str> {
        self.optional_string(name)?.ok_or_else(|| {
            QueryError::invalid_argument(format!("missing required query parameter '{name}'"))
        })
    }

    pub fn optional_u32(&self, name: &str) -> QueryResult<Option<u32>> {
        self.optional_string(name)?
            .map(|value| {
                value.parse::<u32>().map_err(|_| {
                    QueryError::invalid_argument(format!(
                        "invalid u32 value '{value}' for parameter '{name}'"
                    ))
                })
            })
            .transpose()
    }

    pub fn required_u32(&self, name: &str) -> QueryResult<u32> {
        self.optional_u32(name)?.ok_or_else(|| {
            QueryError::invalid_argument(format!("missing required query parameter '{name}'"))
        })
    }

    pub fn optional_u64(&self, name: &str) -> QueryResult<Option<u64>> {
        self.optional_string(name)?
            .map(|value| {
                value.parse::<u64>().map_err(|_| {
                    QueryError::invalid_argument(format!(
                        "invalid u64 value '{value}' for parameter '{name}'"
                    ))
                })
            })
            .transpose()
    }

    pub fn required_u64(&self, name: &str) -> QueryResult<u64> {
        self.optional_u64(name)?.ok_or_else(|| {
            QueryError::invalid_argument(format!("missing required query parameter '{name}'"))
        })
    }

    pub fn optional_i64(&self, name: &str) -> QueryResult<Option<i64>> {
        self.optional_string(name)?
            .map(|value| {
                value.parse::<i64>().map_err(|_| {
                    QueryError::invalid_argument(format!(
                        "invalid i64 value '{value}' for parameter '{name}'"
                    ))
                })
            })
            .transpose()
    }

    pub fn required_i64(&self, name: &str) -> QueryResult<i64> {
        self.optional_i64(name)?.ok_or_else(|| {
            QueryError::invalid_argument(format!("missing required query parameter '{name}'"))
        })
    }

    pub fn optional_bool(&self, name: &str) -> QueryResult<Option<bool>> {
        self.optional_string(name)?
            .map(|value| {
                if value == "1" || value.eq_ignore_ascii_case("true") || value.eq_ignore_ascii_case("yes") {
                    Ok(true)
                } else if value == "0"
                    || value.eq_ignore_ascii_case("false")
                    || value.eq_ignore_ascii_case("no")
                {
                    Ok(false)
                } else {
                    Err(QueryError::invalid_argument(format!(
                        "invalid bool value '{value}' for parameter '{name}'"
                    )))
                }
            })
            .transpose()
    }

    pub fn required_bool(&self, name: &str) -> QueryResult<bool> {
        self.optional_bool(name)?.ok_or_else(|| {
            QueryError::invalid_argument(format!("missing required query parameter '{name}'"))
        })
    }
}

pub struct QueryOutput<'a> {
    write: ffi::HLogQueryWriteFn,
    opaque: *mut c_void,
    error: Option<QueryError>,
    _lifetime: std::marker::PhantomData<&'a mut ()>,
}

impl QueryOutput<'_> {
    pub fn print(&mut self, args: fmt::Arguments<'_>) -> QueryResult<()> {
        let text = args.to_string();
        self.write_text(&text)
    }

    pub fn write_text(&mut self, text: &str) -> QueryResult<()> {
        let rc = unsafe { (self.write)(self.opaque, text.as_ptr(), text.len()) };
        if rc == HLOG_OK {
            Ok(())
        } else {
            let error = QueryError {
                code: rc,
                message: format!("hlog query output writer failed with status {rc}"),
            };
            self.error = Some(error.clone());
            Err(error)
        }
    }
}

impl fmt::Write for QueryOutput<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        self.write_text(text).map_err(|_| fmt::Error)
    }
}

trait ErasedQueryHandler: Send + Sync {
    fn call(&self, request: &QueryRequest<'_>, output: &mut QueryOutput<'_>) -> QueryResult<()>;
}

impl<F> ErasedQueryHandler for F
where
    F: Fn(&QueryRequest<'_>, &mut QueryOutput<'_>) -> QueryResult<()> + Send + Sync,
{
    fn call(&self, request: &QueryRequest<'_>, output: &mut QueryOutput<'_>) -> QueryResult<()> {
        self(request, output)
    }
}

struct QueryHandlerHolder {
    handler: Box<dyn ErasedQueryHandler>,
}

unsafe extern "C" fn query_trampoline(
    opaque: *mut c_void,
    request: *const ffi::HLogQueryRequest,
    write: ffi::HLogQueryWriteFn,
    write_opaque: *mut c_void,
) -> i32 {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let holder = (opaque as *mut QueryHandlerHolder)
            .as_ref()
            .ok_or_else(|| QueryError::internal("query callback context is NULL"))?;
        let request = QueryRequest::from_raw(request)?;
        let mut output = QueryOutput {
            write,
            opaque: write_opaque,
            error: None,
            _lifetime: std::marker::PhantomData,
        };
        holder.handler.call(&request, &mut output)?;
        if let Some(error) = output.error {
            return Err(error);
        }
        Ok(())
    }));

    match result {
        Ok(Ok(())) => HLOG_OK,
        Ok(Err(error)) => set_query_error(&error),
        Err(_) => set_query_error(&QueryError::internal("Rust query callback panicked")),
    }
}

fn set_query_error(error: &QueryError) -> i32 {
    let message = error.message.replace('\0', "\\0");
    let message = CString::new(message).unwrap_or_else(|_| CString::new("invalid query error").unwrap());
    unsafe { ffi::hlog_query_set_error(error.code, message.as_ptr()) }
}

pub struct QueryRegistration {
    handle: u64,
    context: NonNull<QueryHandlerHolder>,
}

unsafe impl Send for QueryRegistration {}
unsafe impl Sync for QueryRegistration {}

impl QueryRegistration {
    pub fn handle(&self) -> u64 {
        self.handle
    }
}

impl Drop for QueryRegistration {
    fn drop(&mut self) {
        let rc = unsafe { ffi::hlog_query_unregister(self.handle) };
        if rc == HLOG_OK {
            unsafe { drop(Box::from_raw(self.context.as_ptr())) };
        }
        /* On failure, leak the context rather than risk a callback UAF. */
    }
}

pub struct QueryBuilder {
    module: String,
    instance: String,
    name: String,
    description: String,
    params: Vec<QueryParam>,
}

pub fn register_query(
    module: impl Into<String>,
    instance: impl Into<String>,
    name: impl Into<String>,
) -> QueryBuilder {
    QueryBuilder {
        module: module.into(),
        instance: instance.into(),
        name: name.into(),
        description: String::new(),
        params: Vec::new(),
    }
}

impl QueryBuilder {
    pub fn description(mut self, description: impl Into<String>) -> Self {
        self.description = description.into();
        self
    }

    pub fn required_string(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::String, description).required());
        self
    }

    pub fn optional_string(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::String, description));
        self
    }

    pub fn required_u32(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::U32, description).required());
        self
    }

    pub fn optional_u32(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::U32, description));
        self
    }

    pub fn repeatable_u32(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::U32, description).repeatable());
        self
    }

    pub fn optional_u64(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::U64, description));
        self
    }

    pub fn required_u64(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::U64, description).required());
        self
    }

    pub fn optional_i64(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::I64, description));
        self
    }

    pub fn required_i64(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::I64, description).required());
        self
    }

    pub fn optional_bool(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::Bool, description));
        self
    }

    pub fn required_bool(mut self, name: impl Into<String>, description: impl Into<String>) -> Self {
        self.params.push(QueryParam::new(name, QueryParamType::Bool, description).required());
        self
    }

    pub fn register<F>(self, handler: F) -> LogResult<QueryRegistration>
    where
        F: Fn(&QueryRequest<'_>, &mut QueryOutput<'_>) -> QueryResult<()> + Send + Sync + 'static,
    {
        ensure_runtime()?;
        let module = CString::new(self.module)?;
        let instance = CString::new(self.instance)?;
        let name = CString::new(self.name)?;
        let description = CString::new(self.description)?;
        let strings: Vec<(CString, CString)> = self
            .params
            .iter()
            .map(|param| Ok((CString::new(param.name.clone())?, CString::new(param.description.clone())?)))
            .collect::<Result<_, std::ffi::NulError>>()?;
        let raw_params: Vec<ffi::HLogQueryParamSpec> = self
            .params
            .iter()
            .zip(&strings)
            .map(|(param, (name, description))| ffi::HLogQueryParamSpec {
                name: name.as_ptr(),
                param_type: param.param_type as u32,
                flags: (if param.required { HLOG_QUERY_PARAM_REQUIRED } else { 0 })
                    | (if param.repeatable { HLOG_QUERY_PARAM_REPEATABLE } else { 0 }),
                description: description.as_ptr(),
            })
            .collect();
        let mut context = Box::new(QueryHandlerHolder {
            handler: Box::new(handler),
        });
        let handle = unsafe {
            ffi::hlog_query_register(
                module.as_ptr(),
                instance.as_ptr(),
                name.as_ptr(),
                description.as_ptr(),
                if raw_params.is_empty() { std::ptr::null() } else { raw_params.as_ptr() },
                raw_params.len(),
                Some(query_trampoline),
                (&mut *context as *mut QueryHandlerHolder).cast(),
            )
        };
        if handle == 0 {
            return Err(last_error_boxed());
        }
        let context = NonNull::new(Box::into_raw(context)).expect("Box never yields NULL");
        Ok(QueryRegistration { handle, context })
    }
}

#[derive(Debug, Clone)]
pub struct LogConfig {
    pub base_dir: PathBuf,
    pub file_name: String,
    pub default_level: LogLevel,
    pub truncate: bool,
    pub rotation_size: u64,
    /// Number of rotated files collected into each compressed archive batch.
    pub max_files: u32,
    pub flush_interval: Option<Duration>,
}

impl Default for LogConfig {
    fn default() -> Self {
        Self {
            base_dir: PathBuf::from("./logs"),
            file_name: "qemu_device.log".to_string(),
            default_level: LogLevel::Info,
            truncate: true,
            rotation_size: DEFAULT_ROTATION_SIZE,
            max_files: DEFAULT_MAX_FILES,
            flush_interval: Some(DEFAULT_FLUSH_INTERVAL),
        }
    }
}

#[derive(Debug)]
pub struct ModuleLogger {
    handle: NonNull<ffi::HLogLogger>,
    module_name: String,
    instance_id: usize,
}

unsafe impl Send for ModuleLogger {}
unsafe impl Sync for ModuleLogger {}

impl ModuleLogger {
    fn from_handle(handle: *mut ffi::HLogLogger) -> LogResult<SharedLogger> {
        let handle = NonNull::new(handle).ok_or_else(last_error_boxed)?;
        let module_name = ffi_string(|buf, cap| unsafe {
            ffi::hlog_logger_module_name(handle.as_ptr(), buf, cap)
        })?
        .ok_or_else(|| boxed_err("logger has no module name"))?;
        let instance_id = unsafe { ffi::hlog_logger_instance_id(handle.as_ptr()) };

        Ok(Arc::new(Self {
            handle,
            module_name,
            instance_id,
        }))
    }

    pub fn name(&self) -> &str {
        &self.module_name
    }

    pub fn instance_id(&self) -> usize {
        self.instance_id
    }

    pub fn configured_name(&self) -> Option<String> {
        ffi_string(|buf, cap| unsafe {
            ffi::hlog_logger_configured_name(self.handle.as_ptr(), buf, cap)
        })
        .ok()
        .flatten()
    }

    pub fn display_name(&self) -> Option<String> {
        ffi_string(|buf, cap| unsafe {
            ffi::hlog_logger_display_name(self.handle.as_ptr(), buf, cap)
        })
        .ok()
        .flatten()
    }

    /*
     * Compatibility helper: all logical loggers now share the same physical
     * process-global log file, so every ModuleLogger returns the same path.
     */
    pub fn file_name(&self) -> Option<String> {
        current_log_file().ok().flatten()
    }

    pub fn level(&self) -> LogLevel {
        LogLevel::from_raw(unsafe { ffi::hlog_logger_level(self.handle.as_ptr()) })
    }

    #[inline]
    pub fn should_log(&self, level: LogLevel) -> bool {
        unsafe { ffi::hlog_enabled(self.handle.as_ptr(), level as u32) }
    }

    pub fn set_level(&self, level: LogLevel) {
        let _ = check_rc(unsafe { ffi::hlog_logger_set_level(self.handle.as_ptr(), level as u32) });
    }

    pub fn disable(&self) {
        self.set_level(LogLevel::Off);
    }

    pub fn flush(&self) {
        let _ = check_rc(unsafe { ffi::hlog_flush() });
    }

    pub fn config_name(&self, new_name: &str) -> LogResult<()> {
        let new_name = CString::new(new_name)?;
        check_rc(unsafe { ffi::hlog_logger_config_name(self.handle.as_ptr(), new_name.as_ptr()) })
    }

    pub fn raw_handle(&self) -> *mut std::ffi::c_void {
        self.handle.as_ptr().cast()
    }

    pub fn trace(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Trace, args);
    }

    pub fn debug(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Debug, args);
    }

    pub fn info(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Info, args);
    }

    pub fn warn(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Warn, args);
    }

    pub fn error(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Error, args);
    }

    pub fn critical(&self, args: fmt::Arguments<'_>) {
        self.write_raw(LogLevel::Critical, args);
    }

    pub fn hexdump(&self, level: LogLevel, data: &[u8]) {
        self.hexdump_context(level, "", "", 0, data);
    }

    pub fn hexdump_format(&self, level: LogLevel, format: HexdumpFormat, data: &[u8]) {
        self.hexdump_format_context(level, format, "", "", 0, data, false);
    }

    #[doc(hidden)]
    pub fn hexdump_format_context(
        &self, level: LogLevel, format: HexdumpFormat, function: &str,
        file: &str, line: u32, data: &[u8], console_only: bool,
    ) {
        if level == LogLevel::Off || (!console_only && !self.should_log(level)) { return; }
        let write = if console_only { ffi::hlog_console_hexdump_ex } else { ffi::hlog_hexdump_ex };
        let _ = check_rc(unsafe { write(
            self.handle.as_ptr(), level as u32,
            function.as_ptr(), function.len(), file.as_ptr(), file.len(), line,
            data.as_ptr(), data.len(), format as u32,
        ) });
    }

    /// Console only: bypasses file levels and mirroring. Formatting can panic
    /// in user Display implementations, just like the ordinary logging macros.
    #[doc(hidden)]
    pub fn console_write_context(
        &self, level: LogLevel, function: &str, file: &str, line: u32,
        args: fmt::Arguments<'_>,
    ) {
        if level == LogLevel::Off { return; }
        with_format_buffer(args, |msg| {
            let _ = unsafe { ffi::hlog_console_write(
                self.handle.as_ptr(), level as u32,
                function.as_ptr(), function.len(), file.as_ptr(), file.len(), line,
                msg.as_ptr(), msg.len(),
            ) };
        });
    }

    #[doc(hidden)]
    pub fn console_hexdump_context(
        &self, level: LogLevel, function: &str, file: &str, line: u32, data: &[u8],
    ) {
        if level == LogLevel::Off { return; }
        let _ = unsafe { ffi::hlog_console_hexdump(
            self.handle.as_ptr(), level as u32,
            function.as_ptr(), function.len(), file.as_ptr(), file.len(), line,
            data.as_ptr(), data.len(),
        ) };
    }

    fn write_raw(&self, level: LogLevel, args: fmt::Arguments<'_>) {
        if !self.should_log(level) {
            return;
        }
        with_format_buffer(args, |msg| {
            let _ = self.write_message(level, "", "", 0, msg);
        });
    }

    #[doc(hidden)]
    pub fn write_context(
        &self,
        level: LogLevel,
        function: &str,
        file: &str,
        line: u32,
        args: fmt::Arguments<'_>,
    ) {
        if !self.should_log(level) {
            return;
        }
        with_format_buffer(args, |msg| {
            let _ = self.write_message(level, function, file, line, msg);
        });
    }

    #[doc(hidden)]
    pub fn hexdump_context(&self, level: LogLevel, function: &str, file: &str, line: u32, data: &[u8]) {
        if !self.should_log(level) {
            return;
        }

        let _ = check_rc(unsafe {
            ffi::hlog_hexdump(
                self.handle.as_ptr(),
                level as u32,
                function.as_ptr(),
                function.len(),
                file.as_ptr(),
                file.len(),
                line,
                data.as_ptr(),
                data.len(),
            )
        });
    }

    fn write_message(
        &self,
        level: LogLevel,
        function: &str,
        file: &str,
        line: u32,
        message: &str,
    ) -> LogResult<()> {
        check_rc(unsafe {
            ffi::hlog_write(
                self.handle.as_ptr(),
                level as u32,
                function.as_ptr(),
                function.len(),
                file.as_ptr(),
                file.len(),
                line,
                message.as_ptr(),
                message.len(),
            )
        })
    }
}

pub fn init<P: Into<PathBuf>>(
    base_dir: P,
    fallback_file: &str,
    fallback_level: LogLevel,
) -> LogResult<()> {
    init_with_options(base_dir, fallback_file, fallback_level, true)
}

pub fn init_with_options<P: Into<PathBuf>>(
    base_dir: P,
    fallback_file: &str,
    fallback_level: LogLevel,
    truncate: bool,
) -> LogResult<()> {
    init_with_config(LogConfig {
        base_dir: base_dir.into(),
        file_name: fallback_file.to_string(),
        default_level: fallback_level,
        truncate,
        ..LogConfig::default()
    })
}

pub fn init_with_config(config: LogConfig) -> LogResult<()> {
    let runtime_abi = unsafe { ffi::hlog_abi_version() };
    if runtime_abi != ABI_VERSION {
        return Err(boxed_err(format!(
            "hlog ABI mismatch: client={ABI_VERSION}, runtime={runtime_abi}"
        )));
    }

    let base_dir = CString::new(config.base_dir.to_string_lossy().as_bytes())?;
    let file_name = CString::new(config.file_name)?;
    let flush_interval_ms = config
        .flush_interval
        .map(|d| d.as_millis().min(u32::MAX as u128) as u32)
        .unwrap_or(0);

    let raw = ffi::HLogConfig {
        struct_size: std::mem::size_of::<ffi::HLogConfig>() as u32,
        abi_version: ABI_VERSION,
        base_dir: base_dir.as_ptr(),
        file_name: file_name.as_ptr(),
        default_level: config.default_level as u32,
        truncate: u8::from(config.truncate),
        reserved: [0; 7],
        rotation_size_bytes: config.rotation_size,
        max_files: config.max_files,
        flush_interval_ms,
    };

    check_rc(unsafe { ffi::hlog_init(&raw) })
}

pub fn load_level_config(path: impl AsRef<Path>) -> LogResult<()> {
    ensure_runtime()?;
    let path = CString::new(path.as_ref().to_string_lossy().as_bytes())?;
    check_rc(unsafe { ffi::hlog_load_level_config(path.as_ptr()) })
}

pub fn set_level_rule(target: &str, level: LogLevel) -> LogResult<()> {
    ensure_runtime()?;
    let target = CString::new(target)?;
    check_rc(unsafe { ffi::hlog_set_level_rule(target.as_ptr(), level as u32) })
}

pub fn create_logger(file_name: &str, level: LogLevel) -> LogResult<SharedLogger> {
    let module_name = infer_module_name(file_name);
    create_module_logger(&module_name, level)
}

pub fn create_named_logger(
    module_name: &str,
    file_name: &str,
    level: LogLevel,
) -> LogResult<SharedLogger> {
    let stem = infer_module_name(file_name);
    if stem == module_name {
        create_module_logger(module_name, level)
    } else {
        create_module_logger_with_name(module_name, &stem, level)
    }
}

pub fn create_module_logger(module_name: &str, level: LogLevel) -> LogResult<SharedLogger> {
    register_logger(module_name, None, level)
}

pub fn create_module_logger_with_name(
    module_name: &str,
    configured_name: &str,
    level: LogLevel,
) -> LogResult<SharedLogger> {
    register_logger(module_name, Some(configured_name), level)
}

fn register_logger(
    module_name: &str,
    configured_name: Option<&str>,
    level: LogLevel,
) -> LogResult<SharedLogger> {
    ensure_runtime()?;
    let module_name = CString::new(module_name)?;
    let configured_name = configured_name.map(CString::new).transpose()?;

    let handle = unsafe {
        ffi::hlog_register_logger(
            module_name.as_ptr(),
            configured_name
                .as_ref()
                .map(|s| s.as_ptr())
                .unwrap_or(std::ptr::null()),
            level as u32,
        )
    };

    ModuleLogger::from_handle(handle)
}

pub fn config_name(logger: &SharedLogger, new_name: &str) -> LogResult<()> {
    logger.config_name(new_name)
}

pub fn get_logger(module_name: &str) -> Option<SharedLogger> {
    let module_name = CString::new(module_name).ok()?;
    let handle = unsafe { ffi::hlog_get_logger(module_name.as_ptr()) };
    ModuleLogger::from_handle(handle).ok()
}

pub fn get_module_loggers(module_name: &str) -> Vec<SharedLogger> {
    let Ok(module_name) = CString::new(module_name) else {
        return Vec::new();
    };
    let count = unsafe { ffi::hlog_module_logger_count(module_name.as_ptr()) };
    (0..count)
        .filter_map(|index| {
            let handle = unsafe { ffi::hlog_module_logger_at(module_name.as_ptr(), index) };
            ModuleLogger::from_handle(handle).ok()
        })
        .collect()
}

pub fn default_logger() -> SharedLogger {
    ensure_runtime().expect("hlog runtime is unavailable");
    ModuleLogger::from_handle(unsafe { ffi::hlog_default_logger() })
        .expect("hlog default logger is unavailable")
}

pub fn resolve_logger(logger: Option<SharedLogger>) -> SharedLogger {
    logger.unwrap_or_else(default_logger)
}

pub fn set_default_level(level: LogLevel) {
    let _ = check_rc(unsafe { ffi::hlog_set_default_level(level as u32) });
}

pub fn set_module_level(module_name: &str, level: LogLevel) -> bool {
    let Ok(module_name) = CString::new(module_name) else {
        return false;
    };
    unsafe { ffi::hlog_set_module_level(module_name.as_ptr(), level as u32) == 0 }
}

pub fn disable_module(module_name: &str) -> bool {
    set_module_level(module_name, LogLevel::Off)
}

pub fn set_all_levels(level: LogLevel) {
    let _ = check_rc(unsafe { ffi::hlog_set_all_levels(level as u32) });
}

pub fn disable_all() {
    set_all_levels(LogLevel::Off);
}

pub fn flush_all() {
    let _ = check_rc(unsafe { ffi::hlog_flush() });
}

pub fn set_flush_interval(interval: Option<Duration>) -> LogResult<()> {
    let ms = interval
        .map(|d| d.as_millis().min(u32::MAX as u128) as u32)
        .unwrap_or(0);
    check_rc(unsafe { ffi::hlog_set_flush_interval_ms(ms) })
}

pub fn shutdown() {
    unsafe { ffi::hlog_shutdown() };
}

pub fn module_names() -> Vec<String> {
    let count = unsafe { ffi::hlog_module_count() };
    (0..count)
        .filter_map(|index| {
            ffi_string(|buf, cap| unsafe { ffi::hlog_module_name_at(index, buf, cap) })
                .ok()
                .flatten()
        })
        .collect()
}

pub fn current_log_file() -> LogResult<Option<String>> {
    ffi_string(|buf, cap| unsafe { ffi::hlog_file_path(buf, cap) })
}

fn ensure_runtime() -> LogResult<()> {
    let runtime_abi = unsafe { ffi::hlog_abi_version() };
    if runtime_abi != ABI_VERSION {
        return Err(boxed_err(format!(
            "hlog ABI mismatch: client={ABI_VERSION}, runtime={runtime_abi}"
        )));
    }
    check_rc(unsafe { ffi::hlog_init_default() })
}

fn infer_module_name(file_name: &str) -> String {
    Path::new(file_name)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or(file_name)
        .to_string()
}

fn with_format_buffer<R>(args: fmt::Arguments<'_>, f: impl FnOnce(&str) -> R) -> R {
    FORMAT_BUFFER.with(|cell| {
        if let Ok(mut buffer) = cell.try_borrow_mut() {
            buffer.clear();
            // Formatting into String is infallible in practice because its
            // fmt::Write implementation only fails on allocation panic.
            fmt::write(&mut *buffer, args).expect("formatting into String failed");
            f(&buffer)
        } else {
            // Re-entrant logging on the same thread.
            let temporary = args.to_string();
            f(&temporary)
        }
    })
}

fn check_rc(rc: i32) -> LogResult<()> {
    if rc == 0 {
        Ok(())
    } else {
        Err(last_error_boxed())
    }
}

fn last_error_boxed() -> Box<dyn std::error::Error + Send + Sync> {
    let message = unsafe {
        let ptr = ffi::hlog_last_error();
        if ptr.is_null() {
            "unknown hlog error".to_string()
        } else {
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    };
    boxed_err(message)
}

fn boxed_err(msg: impl Into<String>) -> Box<dyn std::error::Error + Send + Sync> {
    Box::new(std::io::Error::other(msg.into()))
}

fn ffi_string(
    get: impl Fn(*mut std::os::raw::c_char, usize) -> usize,
) -> LogResult<Option<String>> {
    let required = get(std::ptr::null_mut(), 0);
    if required == 0 {
        return Ok(None);
    }

    let mut buf = vec![0u8; required];
    let actual = get(buf.as_mut_ptr().cast(), buf.len());
    if actual == 0 {
        return Ok(None);
    }
    if actual > buf.len() {
        buf.resize(actual, 0);
        let retry = get(buf.as_mut_ptr().cast(), buf.len());
        if retry == 0 || retry > buf.len() {
            return Err(last_error_boxed());
        }
    }

    let nul = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    Ok(Some(String::from_utf8_lossy(&buf[..nul]).into_owned()))
}

/// Stateless Rust-side proxy for the process-global runtime living in
/// libhlog.so. This preserves the old `LogCenter::global()` style
/// without putting any physical logging state into model .so files.
pub struct LogCenter;

static CLIENT_LOG_CENTER: LogCenter = LogCenter;

impl LogCenter {
    pub fn init<P: Into<PathBuf>>(
        base_dir: P,
        fallback_file: &str,
        fallback_level: LogLevel,
    ) -> LogResult<&'static Self> {
        crate::init(base_dir, fallback_file, fallback_level)?;
        Ok(Self::global())
    }

    pub fn init_with_options<P: Into<PathBuf>>(
        base_dir: P,
        fallback_file: &str,
        fallback_level: LogLevel,
        truncate: bool,
    ) -> LogResult<&'static Self> {
        crate::init_with_options(base_dir, fallback_file, fallback_level, truncate)?;
        Ok(Self::global())
    }

    pub fn init_with_config(config: LogConfig) -> LogResult<&'static Self> {
        crate::init_with_config(config)?;
        Ok(Self::global())
    }

    pub fn load_level_config(&self, path: impl AsRef<Path>) -> LogResult<()> {
        crate::load_level_config(path)
    }

    pub fn set_level_rule(&self, target: &str, level: LogLevel) -> LogResult<()> {
        crate::set_level_rule(target, level)
    }

    pub fn global() -> &'static Self {
        ensure_runtime().expect("hlog runtime is unavailable");
        &CLIENT_LOG_CENTER
    }

    pub fn default_logger(&self) -> SharedLogger {
        crate::default_logger()
    }

    pub fn resolve_logger(&self, logger: Option<SharedLogger>) -> SharedLogger {
        crate::resolve_logger(logger)
    }

    pub fn create_logger(&self, file_name: &str, level: LogLevel) -> LogResult<SharedLogger> {
        crate::create_logger(file_name, level)
    }

    pub fn create_named_logger(
        &self,
        module_name: &str,
        file_name: &str,
        level: LogLevel,
    ) -> LogResult<SharedLogger> {
        crate::create_named_logger(module_name, file_name, level)
    }

    pub fn create_module_logger(
        &self,
        module_name: &str,
        configured_name: Option<&str>,
        level: LogLevel,
    ) -> LogResult<SharedLogger> {
        match configured_name {
            Some(name) => crate::create_module_logger_with_name(module_name, name, level),
            None => crate::create_module_logger(module_name, level),
        }
    }

    pub fn config_logger_name(
        &self,
        logger: &ModuleLogger,
        new_name: &str,
    ) -> LogResult<()> {
        logger.config_name(new_name)
    }

    pub fn get_logger(&self, module_name: &str) -> Option<SharedLogger> {
        crate::get_logger(module_name)
    }

    pub fn get_module_loggers(&self, module_name: &str) -> Vec<SharedLogger> {
        crate::get_module_loggers(module_name)
    }

    pub fn set_default_level(&self, level: LogLevel) {
        crate::set_default_level(level)
    }

    pub fn set_module_level(&self, module_name: &str, level: LogLevel) -> bool {
        crate::set_module_level(module_name, level)
    }

    pub fn disable_module(&self, module_name: &str) -> bool {
        crate::disable_module(module_name)
    }

    pub fn set_all_levels(&self, level: LogLevel) {
        crate::set_all_levels(level)
    }

    pub fn disable_all(&self) {
        crate::disable_all()
    }

    pub fn flush_all(&self) {
        crate::flush_all()
    }

    pub fn module_names(&self) -> Vec<String> {
        crate::module_names()
    }
}

#[macro_export]
macro_rules! ltrace {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Trace) {
            __logger.write_context(
                $crate::LogLevel::Trace,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! ldebug {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Debug) {
            __logger.write_context(
                $crate::LogLevel::Debug,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! linfo {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Info) {
            __logger.write_context(
                $crate::LogLevel::Info,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! lwarn {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Warn) {
            __logger.write_context(
                $crate::LogLevel::Warn,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! lerror {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Error) {
            __logger.write_context(
                $crate::LogLevel::Error,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! lcritical {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        if __logger.should_log($crate::LogLevel::Critical) {
            __logger.write_context(
                $crate::LogLevel::Critical,
                function_name!(),
                file!(),
                line!(),
                format_args!($($arg)*)
            );
        }
    }};
}

#[macro_export]
macro_rules! lhexdump {
    ($logger:expr, $level:expr, $format:expr, $data:expr) => {{
        let hlog_logger_value = &$logger;
        let hlog_level_value = $level;
        if hlog_logger_value.should_log(hlog_level_value) {
            let hlog_data_value = &$data;
            let hlog_bytes: &[u8] = hlog_data_value.as_ref();
            hlog_logger_value.hexdump_format_context(
                hlog_level_value, $format, function_name!(), file!(), line!(), hlog_bytes, false,
            );
        }
    }};
    ($logger:expr, $level:expr, $data:expr) => {{
        let hlog_logger_value = &$logger;
        let hlog_level_value = $level;
        if hlog_logger_value.should_log(hlog_level_value) {
            let hlog_data_value = &$data;
            let hlog_bytes: &[u8] = hlog_data_value.as_ref();
            hlog_logger_value.hexdump_context(
                hlog_level_value,
                function_name!(),
                file!(),
                line!(),
                hlog_bytes,
            );
        }
    }};
}

#[macro_export]
macro_rules! lhexdump_trace {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Trace, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Trace, $format, $data)
    };
}

#[macro_export]
macro_rules! lhexdump_debug {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Debug, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Debug, $format, $data)
    };
}

#[macro_export]
macro_rules! lhexdump_info {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Info, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Info, $format, $data)
    };
}

#[macro_export]
macro_rules! lhexdump_warn {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Warn, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Warn, $format, $data)
    };
}

#[macro_export]
macro_rules! lhexdump_error {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Error, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Error, $format, $data)
    };
}

#[macro_export]
macro_rules! lhexdump_critical {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lhexdump!($logger, $crate::LogLevel::Critical, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lhexdump!($crate::default_logger(), $crate::LogLevel::Critical, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_trace {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Trace, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Trace, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_debug {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Debug, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Debug, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_info {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Info, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Info, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_warn {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Warn, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Warn, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_error {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Error, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Error, $format, $data)
    };
}

#[macro_export]
macro_rules! lconsole_hexdump_critical {
    ($logger:expr, $format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($logger, $crate::LogLevel::Critical, $format, $data)
    };
    ($format:expr, $data:expr) => {
        $crate::lconsole_hexdump!($crate::default_logger(), $crate::LogLevel::Critical, $format, $data)
    };
}

#[macro_export]
macro_rules! lpanic {
    ($logger:expr, $($arg:tt)*) => {{
        let __logger = &$logger;
        let __msg = format!(
            "PANIC at {}:{}: {}",
            file!(),
            line!(),
            format_args!($($arg)*)
        );

        if __logger.should_log($crate::LogLevel::Error) {
            __logger.write_context(
                $crate::LogLevel::Error,
                function_name!(),
                file!(),
                line!(),
                format_args!("{}", __msg)
            );
            __logger.flush();
        }

        panic!("{}", __msg);
    }};
}
