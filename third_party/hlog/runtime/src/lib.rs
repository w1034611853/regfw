use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::{c_void, CStr, CString},
    fmt::Write as _,
    fs::File,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    ptr,
    slice,
    str,
    sync::{
        atomic::{AtomicU64, AtomicU8, Ordering},
        mpsc::{self, RecvTimeoutError, Sender},
        Arc, Condvar, Mutex, OnceLock, RwLock,
    },
    thread::{self, JoinHandle},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use ctor::ctor;
use spdlog::{
    formatter::{pattern, Formatter, FormatterContext, PatternFormatter},
    prelude::*,
    sink::{RotatingFileSink, RotationPolicy, Sink, StdStreamSink},
    terminal_style::StyleMode,
    Level, LevelFilter, Logger, Record, StringBuf,
};

mod console;
use console::console_output;
mod hexdump;
use hexdump::{HexdumpFormat, HexdumpRows};

const HLOG_ABI_VERSION: u32 = 3;
const DEFAULT_LOGGER_NAME: &str = "__default__";
const DEFAULT_BASE_DIR: &str = "./logs";
const DEFAULT_FILE_NAME: &str = "qemu_device.log";
const DEFAULT_ROTATION_SIZE: u64 = 100 * 1024 * 1024;
const DEFAULT_MAX_FILES: u32 = 10;
const DEFAULT_FLUSH_INTERVAL_MS: u32 = 2_000;
const ARCHIVE_CHECK_INTERVAL_BYTES: u64 = 1024 * 1024;
const HLOG_QUERY_PARAM_REQUIRED: u32 = 1 << 0;
const HLOG_QUERY_PARAM_REPEATABLE: u32 = 1 << 1;
const HLOG_QUERY_PARAM_VALID_FLAGS: u32 =
    HLOG_QUERY_PARAM_REQUIRED | HLOG_QUERY_PARAM_REPEATABLE;
const HLOG_QUERY_MAX_PARAMS: usize = 64;
const HLOG_QUERY_MAX_RAW_ARGS: usize = 16 * 1024;
const HLOG_QUERY_MAX_OUTPUT: usize = 1024 * 1024;

const HLOG_OK: i32 = 0;
const HLOG_ERR_INVALID_ARGUMENT: i32 = -1;
const HLOG_ERR_INTERNAL: i32 = -2;
const HLOG_ERR_NOT_FOUND: i32 = -3;
const HLOG_ERR_DUPLICATE: i32 = -4;
const HLOG_ERR_ABI_MISMATCH: i32 = -5;
const HLOG_ERR_BUSY: i32 = -6;
const HLOG_ERR_OUTPUT_TOO_LARGE: i32 = -7;

#[repr(C)]
pub struct HLogConfig {
    pub struct_size: u32,
    pub abi_version: u32,
    pub base_dir: *const std::os::raw::c_char,
    pub file_name: *const std::os::raw::c_char,
    pub default_level: u32,
    pub truncate: u8,
    pub reserved: [u8; 7],
    pub rotation_size_bytes: u64,
    pub max_files: u32,
    pub flush_interval_ms: u32,
}

#[repr(C)]
pub struct HLogLogger {
    _private: [u8; 0],
}

#[repr(C)]
pub struct HLogQueryParamSpec {
    pub name: *const std::os::raw::c_char,
    pub param_type: u32,
    pub flags: u32,
    pub description: *const std::os::raw::c_char,
}

#[repr(C)]
pub struct HLogQueryArg {
    pub name: *const std::os::raw::c_char,
    pub value: *const std::os::raw::c_char,
}

#[repr(C)]
pub struct HLogQueryRequest {
    pub args: *const HLogQueryArg,
    pub arg_count: usize,
}

type HLogQueryWriteFn = unsafe extern "C" fn(*mut c_void, *const u8, usize) -> i32;
type HLogQueryCallback = unsafe extern "C" fn(
    *mut c_void,
    *const HLogQueryRequest,
    HLogQueryWriteFn,
    *mut c_void,
) -> i32;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
enum LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
}

impl LogLevel {
    fn from_ffi(value: u32) -> Result<Self, String> {
        match value {
            0 => Ok(Self::Trace),
            1 => Ok(Self::Debug),
            2 => Ok(Self::Info),
            3 => Ok(Self::Warn),
            4 => Ok(Self::Error),
            5 => Ok(Self::Critical),
            6 => Ok(Self::Off),
            _ => Err(format!("invalid log level {value}")),
        }
    }

    fn from_u8(value: u8) -> Self {
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

    #[inline]
    fn allows(self, level: Self) -> bool {
        self != Self::Off && level != Self::Off && (level as u32) >= (self as u32)
    }
}

impl From<LogLevel> for LevelFilter {
    fn from(value: LogLevel) -> Self {
        match value {
            LogLevel::Trace => LevelFilter::All,
            LogLevel::Debug => LevelFilter::MoreSevereEqual(Level::Debug),
            LogLevel::Info => LevelFilter::MoreSevereEqual(Level::Info),
            LogLevel::Warn => LevelFilter::MoreSevereEqual(Level::Warn),
            LogLevel::Error => LevelFilter::MoreSevereEqual(Level::Error),
            LogLevel::Critical => LevelFilter::MoreSevereEqual(Level::Critical),
            LogLevel::Off => LevelFilter::Off,
        }
    }
}

#[derive(Debug, Clone)]
struct RuntimeConfig {
    base_dir: PathBuf,
    file_name: String,
    default_level: LogLevel,
    truncate: bool,
    rotation_size_bytes: u64,
    max_files: u32,
    flush_interval_ms: u32,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            base_dir: PathBuf::from(DEFAULT_BASE_DIR),
            file_name: DEFAULT_FILE_NAME.to_string(),
            default_level: LogLevel::Info,
            truncate: true,
            rotation_size_bytes: DEFAULT_ROTATION_SIZE,
            max_files: DEFAULT_MAX_FILES,
            flush_interval_ms: DEFAULT_FLUSH_INTERVAL_MS,
        }
    }
}

impl RuntimeConfig {
    fn validate(&self) -> Result<(), String> {
        if self.file_name.trim().is_empty() {
            return Err("file_name must not be empty".to_string());
        }
        if self.rotation_size_bytes == 0 {
            return Err("rotation_size_bytes must be greater than zero".to_string());
        }
        if self.max_files == 0 {
            return Err("max_files must be greater than zero".to_string());
        }
        if self
            .rotation_size_bytes
            .checked_mul(self.max_files as u64)
            .is_none()
        {
            return Err("rotation_size_bytes * max_files is too large".to_string());
        }
        Ok(())
    }

    fn log_path(&self) -> PathBuf {
        let path = Path::new(&self.file_name);
        if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.base_dir.join(path)
        }
    }
}

struct LoggerEntry {
    module_name: String,
    instance_id: usize,
    configured_name: RwLock<Option<String>>,
    fallback_level: LogLevel,
    level: AtomicU8,
    inner: RwLock<Arc<Logger>>,
}

impl LoggerEntry {
    fn new(
        module_name: String,
        instance_id: usize,
        configured_name: Option<String>,
        level: LogLevel,
        inner: Arc<Logger>,
    ) -> Self {
        Self {
            module_name,
            instance_id,
            configured_name: RwLock::new(configured_name),
            fallback_level: level,
            level: AtomicU8::new(level as u8),
            inner: RwLock::new(inner),
        }
    }

    fn configured_name(&self) -> Option<String> {
        self.configured_name.read().unwrap().clone()
    }

    fn effective_label(&self) -> String {
        self.configured_name()
            .unwrap_or_else(|| self.instance_id.to_string())
    }

    fn display_name(&self) -> String {
        if self.module_name == DEFAULT_LOGGER_NAME {
            DEFAULT_LOGGER_NAME.to_string()
        } else {
            format!("{}/{}", self.module_name, self.effective_label())
        }
    }

    #[inline]
    fn level(&self) -> LogLevel {
        LogLevel::from_u8(self.level.load(Ordering::Relaxed))
    }

    #[inline]
    fn should_log(&self, level: LogLevel) -> bool {
        self.level().allows(level)
    }

    fn set_level(&self, level: LogLevel) {
        let inner = self.inner.read().unwrap().clone();
        inner.set_level_filter(level.into());
        self.level.store(level as u8, Ordering::Release);
    }

    fn replace_backend(&self, new_inner: Arc<Logger>) {
        *self.inner.write().unwrap() = new_inner;
    }

    fn write(&self, level: LogLevel, function: &str, message: &str) {
        if !self.should_log(level) {
            return;
        }

        let inner = self.inner.read().unwrap().clone();

        Self::emit(&inner, level, function, message);
    }

    // Both destinations use exactly the same function/payload formatting.
    fn emit(inner: &Arc<Logger>, level: LogLevel, function: &str, message: &str) {
        if function.is_empty() {
            match level {
                LogLevel::Trace => spdlog::trace!(logger: inner, "{}", message),
                LogLevel::Debug => spdlog::debug!(logger: inner, "{}", message),
                LogLevel::Info => spdlog::info!(logger: inner, "{}", message),
                LogLevel::Warn => spdlog::warn!(logger: inner, "{}", message),
                LogLevel::Error => spdlog::error!(logger: inner, "{}", message),
                LogLevel::Critical => spdlog::critical!(logger: inner, "{}", message),
                LogLevel::Off => {}
            }
        } else {
            match level {
                LogLevel::Trace => spdlog::trace!(logger: inner, "[{}] {}", function, message),
                LogLevel::Debug => spdlog::debug!(logger: inner, "[{}] {}", function, message),
                LogLevel::Info => spdlog::info!(logger: inner, "[{}] {}", function, message),
                LogLevel::Warn => spdlog::warn!(logger: inner, "[{}] {}", function, message),
                LogLevel::Error => spdlog::error!(logger: inner, "[{}] {}", function, message),
                LogLevel::Critical => {
                    spdlog::critical!(logger: inner, "[{}] {}", function, message)
                }
                LogLevel::Off => {}
            }
        }
    }
}

#[derive(Debug, Clone, Default)]
struct LevelRules {
    all: Option<LogLevel>,
    modules: HashMap<String, LogLevel>,
    loggers: HashMap<String, LogLevel>,
}

impl LevelRules {
    fn load(path: &Path) -> Result<Self, String> {
        let contents = std::fs::read_to_string(path)
            .map_err(|e| format!("failed to read hlog level config '{}': {e}", path.display()))?;
        Self::parse(&contents, path)
    }

    fn parse(contents: &str, path: &Path) -> Result<Self, String> {
        let mut rules = Self::default();
        let mut rule_count = 0usize;

        for (index, source_line) in contents.lines().enumerate() {
            let line_number = index + 1;
            let line = source_line.split_once('#').map_or(source_line, |(value, _)| value).trim();

            if line.is_empty() {
                continue;
            }

            let (target, level_text) = line.split_once('=').ok_or_else(|| {
                format!("{}:{line_number}: expected 'target = level'", path.display())
            })?;
            if level_text.contains('=') {
                return Err(format!(
                    "{}:{line_number}: expected exactly one '=' separator",
                    path.display()
                ));
            }

            let target = target.trim();
            let level_text = level_text.trim();
            let level = parse_level_text(level_text).ok_or_else(|| {
                format!(
                    "{}:{line_number}: invalid level '{level_text}'; expected trace/debug/info/warn/error/critical/off",
                    path.display()
                )
            })?;

            if target.eq_ignore_ascii_case("all") {
                if rules.all.replace(level).is_some() {
                    return Err(format!("{}:{line_number}: duplicate log target 'all'", path.display()));
                }
            } else if let Some((module_name, logger_name)) = target.split_once('/') {
                if logger_name.contains('/')
                    || !valid_rule_component(module_name)
                    || !valid_rule_component(logger_name)
                {
                    return Err(format!(
                        "{}:{line_number}: invalid logger target '{target}'",
                        path.display()
                    ));
                }
                if rules.loggers.insert(target.to_string(), level).is_some() {
                    return Err(format!(
                        "{}:{line_number}: duplicate log target '{target}'",
                        path.display()
                    ));
                }
            } else {
                if !valid_rule_component(target) {
                    return Err(format!(
                        "{}:{line_number}: invalid module target '{target}'",
                        path.display()
                    ));
                }
                if rules.modules.insert(target.to_string(), level).is_some() {
                    return Err(format!(
                        "{}:{line_number}: duplicate log target '{target}'",
                        path.display()
                    ));
                }
            }
            rule_count += 1;
        }

        if rule_count == 0 {
            return Err(format!("hlog level config '{}' contains no rules", path.display()));
        }
        Ok(rules)
    }

    fn effective(&self, module_name: &str, display_name: &str, fallback: LogLevel) -> LogLevel {
        self.loggers
            .get(display_name)
            .or_else(|| self.modules.get(module_name))
            .copied()
            .or(self.all)
            .unwrap_or(fallback)
    }

    fn set_config_rule(&mut self, target: &str, level: LogLevel) -> Result<(), String> {
        if target.eq_ignore_ascii_case("all") {
            self.all = Some(level);
        } else if let Some((module_name, logger_name)) = target.split_once('/') {
            if logger_name.contains('/')
                || !valid_rule_component(module_name)
                || !valid_rule_component(logger_name)
            {
                return Err(format!("invalid logger target '{target}'"));
            }
            self.loggers.insert(target.to_string(), level);
        } else {
            if !valid_rule_component(target) {
                return Err(format!("invalid module target '{target}'"));
            }
            self.modules.insert(target.to_string(), level);
        }
        Ok(())
    }

    fn set_logger(&mut self, display_name: String, level: LogLevel) {
        self.loggers.insert(display_name, level);
    }

    fn set_module(&mut self, module_name: &str, level: LogLevel) {
        self.modules.insert(module_name.to_string(), level);
        let prefix = format!("{module_name}/");
        self.loggers.retain(|name, _| !name.starts_with(&prefix));
    }

    fn set_all(&mut self, level: LogLevel) {
        self.all = Some(level);
        self.modules.clear();
        self.loggers.clear();
    }
}

fn parse_level_text(value: &str) -> Option<LogLevel> {
    if value.eq_ignore_ascii_case("trace") {
        Some(LogLevel::Trace)
    } else if value.eq_ignore_ascii_case("debug") {
        Some(LogLevel::Debug)
    } else if value.eq_ignore_ascii_case("info") {
        Some(LogLevel::Info)
    } else if value.eq_ignore_ascii_case("warn") || value.eq_ignore_ascii_case("warning") {
        Some(LogLevel::Warn)
    } else if value.eq_ignore_ascii_case("error") {
        Some(LogLevel::Error)
    } else if value.eq_ignore_ascii_case("critical") {
        Some(LogLevel::Critical)
    } else if value.eq_ignore_ascii_case("off") {
        Some(LogLevel::Off)
    } else {
        None
    }
}

fn valid_rule_component(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

fn tokenize_query_args(input: &str) -> Result<Vec<String>, String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut quote = None;
    let mut escaped = false;
    let mut token_started = false;

    for ch in input.chars() {
        if escaped {
            current.push(ch);
            escaped = false;
            token_started = true;
            continue;
        }
        if ch == '\\' {
            escaped = true;
            token_started = true;
            continue;
        }
        if let Some(delimiter) = quote {
            if ch == delimiter {
                quote = None;
            } else {
                current.push(ch);
            }
            token_started = true;
            continue;
        }
        if ch == '\'' || ch == '"' {
            quote = Some(ch);
            token_started = true;
        } else if ch.is_whitespace() {
            if token_started {
                tokens.push(std::mem::take(&mut current));
                token_started = false;
            }
        } else {
            current.push(ch);
            token_started = true;
        }
    }

    if escaped {
        return Err("query arguments end with an incomplete escape".to_string());
    }
    if quote.is_some() {
        return Err("query arguments contain an unterminated quote".to_string());
    }
    if token_started {
        tokens.push(current);
    }
    Ok(tokens)
}

fn parse_query_args(input: &str, specs: &[QueryParamSpec]) -> Result<ParsedQueryArgs, String> {
    let tokens = tokenize_query_args(input)?;
    if tokens.len() > HLOG_QUERY_MAX_PARAMS {
        return Err(format!(
            "query contains {} arguments; maximum is {HLOG_QUERY_MAX_PARAMS}",
            tokens.len()
        ));
    }

    let spec_by_name: HashMap<&str, &QueryParamSpec> =
        specs.iter().map(|spec| (spec.name.as_str(), spec)).collect();
    let mut seen: HashMap<&str, usize> = HashMap::new();
    let mut names = Vec::with_capacity(tokens.len());
    let mut values = Vec::with_capacity(tokens.len());

    for token in tokens {
        let (name, value) = token.split_once('=').ok_or_else(|| {
            format!("invalid query argument '{token}'; expected name=value")
        })?;
        if !valid_rule_component(name) {
            return Err(format!("invalid query parameter name '{name}'"));
        }
        let spec = spec_by_name.get(name).ok_or_else(|| {
            let expected = specs
                .iter()
                .map(|spec| spec.name.as_str())
                .collect::<Vec<_>>()
                .join(", ");
            format!("unknown query parameter '{name}'; expected {expected}")
        })?;
        let count = seen.entry(spec.name.as_str()).or_insert(0);
        if *count != 0 && !spec.repeatable {
            return Err(format!("query parameter '{name}' may only be specified once"));
        }
        *count += 1;
        spec.param_type.validate(name, value)?;
        names.push(CString::new(name).map_err(|_| "query parameter contains NUL".to_string())?);
        values.push(CString::new(value).map_err(|_| "query value contains NUL".to_string())?);
    }

    for spec in specs {
        if spec.required && !seen.contains_key(spec.name.as_str()) {
            return Err(format!("missing required query parameter '{}'", spec.name));
        }
    }
    Ok(ParsedQueryArgs { names, values })
}

#[derive(Default)]
struct ModuleGroup {
    next_instance_id: usize,
    instances: Vec<Arc<LoggerEntry>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
enum QueryParamType {
    String = 0,
    U32 = 1,
    U64 = 2,
    I64 = 3,
    Bool = 4,
}

impl QueryParamType {
    fn from_ffi(value: u32) -> Result<Self, String> {
        match value {
            0 => Ok(Self::String),
            1 => Ok(Self::U32),
            2 => Ok(Self::U64),
            3 => Ok(Self::I64),
            4 => Ok(Self::Bool),
            _ => Err(format!("invalid query parameter type {value}")),
        }
    }

    fn display_name(self) -> &'static str {
        match self {
            Self::String => "string",
            Self::U32 => "u32",
            Self::U64 => "u64",
            Self::I64 => "i64",
            Self::Bool => "bool",
        }
    }

    fn validate(self, name: &str, value: &str) -> Result<(), String> {
        let invalid = || {
            format!(
                "invalid value '{value}' for parameter '{name}'; expected {}",
                self.display_name()
            )
        };

        match self {
            Self::String => Ok(()),
            Self::U32 => value.parse::<u32>().map(|_| ()).map_err(|_| invalid()),
            Self::U64 => value.parse::<u64>().map(|_| ()).map_err(|_| invalid()),
            Self::I64 => value.parse::<i64>().map(|_| ()).map_err(|_| invalid()),
            Self::Bool
                if value == "1"
                    || value == "0"
                    || value.eq_ignore_ascii_case("true")
                    || value.eq_ignore_ascii_case("false")
                    || value.eq_ignore_ascii_case("yes")
                    || value.eq_ignore_ascii_case("no") => Ok(()),
            Self::Bool => Err(invalid()),
        }
    }
}

#[derive(Debug, Clone)]
struct QueryParamSpec {
    name: String,
    param_type: QueryParamType,
    required: bool,
    repeatable: bool,
    description: String,
}

struct QueryLifecycle {
    enabled: bool,
    active_calls: usize,
}

struct QueryEntry {
    target: String,
    description: String,
    params: Vec<QueryParamSpec>,
    callback: HLogQueryCallback,
    opaque: usize,
    lifecycle: Mutex<QueryLifecycle>,
    idle: Condvar,
}

impl QueryEntry {
    fn usage(&self) -> String {
        let mut usage = self.target.clone();
        for param in &self.params {
            let item = format!("{}=<{}>", param.name, param.param_type.display_name());
            if param.required {
                usage.push(' ');
                usage.push_str(&item);
            } else {
                usage.push_str(" [");
                usage.push_str(&item);
                usage.push(']');
            }
            if param.repeatable {
                usage.push_str("...");
            }
        }
        for param in &self.params {
            usage.push_str("\n  ");
            usage.push_str(&param.name);
            usage.push_str(" <");
            usage.push_str(param.param_type.display_name());
            usage.push_str("> ");
            usage.push_str(if param.required { "required" } else { "optional" });
            if param.repeatable {
                usage.push_str(", repeatable");
            }
            if !param.description.is_empty() {
                usage.push_str(" - ");
                usage.push_str(&param.description);
            }
        }
        usage
    }

    fn begin_call(self: &Arc<Self>) -> Result<QueryCallGuard, String> {
        let mut state = self.lifecycle.lock().unwrap();
        if !state.enabled {
            return Err(format!("query '{}' is being unregistered", self.target));
        }
        state.active_calls += 1;
        Ok(QueryCallGuard { entry: self.clone() })
    }

    fn disable_and_wait(&self) {
        let mut state = self.lifecycle.lock().unwrap();
        state.enabled = false;
        while state.active_calls != 0 {
            state = self.idle.wait(state).unwrap();
        }
    }
}

struct QueryCallGuard {
    entry: Arc<QueryEntry>,
}

impl Drop for QueryCallGuard {
    fn drop(&mut self) {
        let mut state = self.entry.lifecycle.lock().unwrap();
        state.active_calls -= 1;
        if state.active_calls == 0 {
            self.entry.idle.notify_all();
        }
    }
}

#[derive(Default)]
struct QueryMaps {
    by_handle: HashMap<u64, Arc<QueryEntry>>,
    by_target: HashMap<String, u64>,
}

struct QueryRegistry {
    next_handle: AtomicU64,
    maps: RwLock<QueryMaps>,
}

impl Default for QueryRegistry {
    fn default() -> Self {
        Self {
            next_handle: AtomicU64::new(1),
            maps: RwLock::new(QueryMaps::default()),
        }
    }
}

#[derive(Debug)]
struct ParsedQueryArgs {
    names: Vec<CString>,
    values: Vec<CString>,
}

impl ParsedQueryArgs {
    fn ffi_args(&self) -> Vec<HLogQueryArg> {
        self.names
            .iter()
            .zip(&self.values)
            .map(|(name, value)| HLogQueryArg {
                name: name.as_ptr(),
                value: value.as_ptr(),
            })
            .collect()
    }
}

struct LimitedQueryWriter {
    write: HLogQueryWriteFn,
    opaque: *mut c_void,
    written: usize,
    overflowed: bool,
    downstream_error: i32,
}

unsafe extern "C" fn limited_query_write(
    opaque: *mut c_void,
    data: *const u8,
    length: usize,
) -> i32 {
    if opaque.is_null() || (length != 0 && data.is_null()) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }

    let state = &mut *(opaque as *mut LimitedQueryWriter);
    if length > HLOG_QUERY_MAX_OUTPUT.saturating_sub(state.written) {
        state.overflowed = true;
        return HLOG_ERR_OUTPUT_TOO_LARGE;
    }

    let rc = (state.write)(state.opaque, data, length);
    if rc == HLOG_OK {
        state.written += length;
    } else {
        state.downstream_error = rc;
    }
    rc
}

impl QueryRegistry {
    fn register(
        &self,
        module: String,
        instance: String,
        name: String,
        description: String,
        params: Vec<QueryParamSpec>,
        callback: HLogQueryCallback,
        opaque: *mut c_void,
    ) -> Result<u64, (i32, String)> {
        for (field, value) in [("module", &module), ("instance", &instance), ("name", &name)] {
            if !valid_rule_component(value) {
                return Err((
                    HLOG_ERR_INVALID_ARGUMENT,
                    format!("query {field} '{value}' contains invalid characters"),
                ));
            }
        }

        let target = format!("{module}/{instance}/{name}");
        let mut maps = self.maps.write().unwrap();
        if maps.by_target.contains_key(&target) {
            return Err((
                HLOG_ERR_DUPLICATE,
                format!("query '{target}' is already registered"),
            ));
        }

        let handle = self.next_handle.fetch_add(1, Ordering::Relaxed);
        if handle == 0 {
            return Err((HLOG_ERR_INTERNAL, "query handle space exhausted".to_string()));
        }

        let entry = Arc::new(QueryEntry {
            target: target.clone(),
            description,
            params,
            callback,
            opaque: opaque as usize,
            lifecycle: Mutex::new(QueryLifecycle {
                enabled: true,
                active_calls: 0,
            }),
            idle: Condvar::new(),
        });
        maps.by_target.insert(target, handle);
        maps.by_handle.insert(handle, entry);
        Ok(handle)
    }

    fn unregister(&self, handle: u64) -> Result<(), (i32, String)> {
        if handle == 0 {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                "query handle must not be zero".to_string(),
            ));
        }

        let entry = {
            let mut maps = self.maps.write().unwrap();
            let Some(entry) = maps.by_handle.remove(&handle) else {
                return Err((
                    HLOG_ERR_NOT_FOUND,
                    format!("query handle {handle} is not registered"),
                ));
            };
            maps.by_target.remove(&entry.target);
            entry
        };

        entry.disable_and_wait();
        Ok(())
    }

    fn entries(&self) -> Vec<Arc<QueryEntry>> {
        let mut entries: Vec<_> = self.maps.read().unwrap().by_handle.values().cloned().collect();
        entries.sort_by(|left, right| left.target.cmp(&right.target));
        entries
    }

    fn find(&self, target: &str) -> Option<Arc<QueryEntry>> {
        let maps = self.maps.read().unwrap();
        let handle = maps.by_target.get(target)?;
        maps.by_handle.get(handle).cloned()
    }

    fn execute(
        &self,
        target: &str,
        raw_args: &str,
        write: HLogQueryWriteFn,
        write_opaque: *mut c_void,
    ) -> Result<(), (i32, String)> {
        if raw_args.len() > HLOG_QUERY_MAX_RAW_ARGS {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                format!("query arguments exceed {HLOG_QUERY_MAX_RAW_ARGS} bytes"),
            ));
        }

        let entry = self.find(target).ok_or_else(|| {
            (HLOG_ERR_NOT_FOUND, format!("query '{target}' is not registered"))
        })?;
        let _call = entry
            .begin_call()
            .map_err(|message| (HLOG_ERR_BUSY, message))?;
        let parsed = parse_query_args(raw_args, &entry.params)
            .map_err(|message| (HLOG_ERR_INVALID_ARGUMENT, message))?;
        let ffi_args = parsed.ffi_args();
        let request = HLogQueryRequest {
            args: if ffi_args.is_empty() {
                ptr::null()
            } else {
                ffi_args.as_ptr()
            },
            arg_count: ffi_args.len(),
        };
        let mut limited = LimitedQueryWriter {
            write,
            opaque: write_opaque,
            written: 0,
            overflowed: false,
            downstream_error: HLOG_OK,
        };

        clear_last_error();
        let rc = unsafe {
            (entry.callback)(
                entry.opaque as *mut c_void,
                &request,
                limited_query_write,
                (&mut limited as *mut LimitedQueryWriter).cast(),
            )
        };

        if limited.overflowed {
            return Err((
                HLOG_ERR_OUTPUT_TOO_LARGE,
                format!("query output exceeds {HLOG_QUERY_MAX_OUTPUT} bytes"),
            ));
        }
        if limited.downstream_error != HLOG_OK {
            return Err((
                limited.downstream_error,
                "query output writer failed".to_string(),
            ));
        }
        if rc != HLOG_OK {
            let message = current_last_error()
                .filter(|value| !value.is_empty())
                .unwrap_or_else(|| format!("query callback returned error {rc}"));
            return Err((rc, message));
        }
        Ok(())
    }
}

enum FlushCommand {
    Wake,
    Stop,
}

struct FlushWorker {
    tx: Sender<FlushCommand>,
    handle: JoinHandle<()>,
}

#[derive(Default)]
struct ArchiveState {
    bytes_since_check: u64,
    sequence: u64,
}

struct ArchiveJob {
    staging_dir: PathBuf,
    staged_paths: Vec<PathBuf>,
    archive_path: PathBuf,
}

enum ArchiveCommand {
    Compress(ArchiveJob),
    Stop,
}

struct ArchiveWorker {
    tx: Sender<ArchiveCommand>,
    handle: JoinHandle<()>,
}

struct LogCenter {
    config: RwLock<RuntimeConfig>,
    sink: RwLock<Arc<RotatingFileSink>>,
    default_logger: Arc<LoggerEntry>,
    modules: RwLock<HashMap<String, ModuleGroup>>,
    level_rules: RwLock<LevelRules>,
    queries: QueryRegistry,
    control_lock: Mutex<()>,
    write_lock: Mutex<()>,
    archive_state: Mutex<ArchiveState>,
    archive_worker: Mutex<Option<ArchiveWorker>>,
    flush_worker: Mutex<Option<FlushWorker>>,
}

static LOG_CENTER: OnceLock<LogCenter> = OnceLock::new();

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

#[ctor]
fn init_log_center_at_load() {
    if let Err(err) = ensure_center() {
        eprintln!("[hlog] fallback initialization failed: {err}");
    }
}

impl LogCenter {
    fn new(config: RuntimeConfig) -> Result<Self, String> {
        config.validate()?;
        let sink = Self::build_sink(&config, config.truncate)?;
        let default_inner = Self::build_logger(
            DEFAULT_LOGGER_NAME,
            config.default_level,
            sink.clone(),
        )?;

        let default_logger = Arc::new(LoggerEntry::new(
            DEFAULT_LOGGER_NAME.to_string(),
            0,
            None,
            config.default_level,
            default_inner,
        ));

        Ok(Self {
            config: RwLock::new(config),
            sink: RwLock::new(sink),
            default_logger,
            modules: RwLock::new(HashMap::new()),
            level_rules: RwLock::new(LevelRules::default()),
            queries: QueryRegistry::default(),
            control_lock: Mutex::new(()),
            write_lock: Mutex::new(()),
            archive_state: Mutex::new(ArchiveState::default()),
            archive_worker: Mutex::new(None),
            flush_worker: Mutex::new(None),
        })
    }

    fn build_sink(
        config: &RuntimeConfig,
        truncate: bool,
    ) -> Result<Arc<RotatingFileSink>, String> {
        let path = config.log_path();

        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| {
                format!("failed to create log directory '{}': {e}", parent.display())
            })?;
        }

        if truncate {
            Self::clear_log_family(&path).map_err(|e| {
                format!("failed to clear log family '{}': {e}", path.display())
            })?;
        }

        RotatingFileSink::builder()
            .base_path(path)
            .rotation_policy(RotationPolicy::FileSize(config.rotation_size_bytes))
            /*
             * Keep one private overflow slot. hlog archives the configured
             * batch before this extra slot can be discarded by the rotating
             * sink, so no oldest log is lost at the 1 GiB boundary.
             */
            .max_files(config.max_files.saturating_add(1) as usize)
            .rotate_on_open(false)
            .formatter(PatternFormatter::new(pattern!(
                "[{time}.{millisecond}] [{level}] [{logger}] [tid={tid}] {payload}{eol}"
            )))
            .build_arc()
            .map_err(|e| format!("failed to build rotating file sink: {e}"))
    }

    fn build_logger(
        display_name: &str,
        level: LogLevel,
        sink: Arc<RotatingFileSink>,
    ) -> Result<Arc<Logger>, String> {
        Logger::builder()
            .name(display_name.to_string())
            .level_filter(level.into())
            .flush_level_filter(LevelFilter::MoreSevereEqual(Level::Warn))
            .sink(sink)
            .sink(console_output()?.mirror.clone())
            .build_arc()
            .map_err(|e| format!("failed to build logger '{display_name}': {e}"))
    }

    fn register_logger(
        &self,
        module_name: &str,
        configured_name: Option<&str>,
        level: LogLevel,
    ) -> Result<Arc<LoggerEntry>, String> {
        let _control = self.control_lock.lock().unwrap();

        let module_name = sanitize_component(module_name);
        if module_name == DEFAULT_LOGGER_NAME {
            return Err(format!("'{DEFAULT_LOGGER_NAME}' is reserved"));
        }

        let configured_name = configured_name
            .filter(|s| !s.is_empty())
            .map(sanitize_component);

        let mut modules = self.modules.write().unwrap();
        let group = modules.entry(module_name.clone()).or_default();

        if let Some(ref name) = configured_name {
            if group
                .instances
                .iter()
                .any(|entry| entry.effective_label() == *name)
            {
                return Err(format!(
                    "duplicate logger name '{name}' under module '{module_name}'"
                ));
            }
        }

        let mut instance_id = group.next_instance_id + 1;
        if configured_name.is_none() {
            while group
                .instances
                .iter()
                .any(|entry| entry.effective_label() == instance_id.to_string())
            {
                instance_id += 1;
            }
        }
        group.next_instance_id = instance_id;

        let label = configured_name
            .clone()
            .unwrap_or_else(|| instance_id.to_string());
        let display_name = format!("{module_name}/{label}");
        let effective_level = self
            .level_rules
            .read()
            .unwrap()
            .effective(&module_name, &display_name, level);
        let sink = self.sink.read().unwrap().clone();
        let inner = Self::build_logger(&display_name, effective_level, sink)?;

        let entry = Arc::new(LoggerEntry::new(
            module_name,
            instance_id,
            configured_name,
            level,
            inner,
        ));
        entry.set_level(effective_level);

        group.instances.push(entry.clone());
        Ok(entry)
    }

    fn config_logger_name(
        &self,
        entry: &LoggerEntry,
        new_name: &str,
    ) -> Result<(), String> {
        if entry.module_name == DEFAULT_LOGGER_NAME {
            return Err("default logger cannot be renamed".to_string());
        }

        let _control = self.control_lock.lock().unwrap();
        let new_name = sanitize_component(new_name);

        let modules = self.modules.read().unwrap();
        let group = modules
            .get(&entry.module_name)
            .ok_or_else(|| format!("module '{}' not found", entry.module_name))?;

        if group.instances.iter().any(|other| {
            !std::ptr::eq(Arc::as_ptr(other), entry as *const LoggerEntry) && other.effective_label() == new_name
        }) {
            return Err(format!(
                "duplicate logger name '{new_name}' under module '{}'",
                entry.module_name
            ));
        }

        let display_name = format!("{}/{}", entry.module_name, new_name);
        let effective_level = self.level_rules.read().unwrap().effective(
            &entry.module_name,
            &display_name,
            entry.fallback_level,
        );
        let sink = self.sink.read().unwrap().clone();
        let new_inner = Self::build_logger(&display_name, effective_level, sink)?;

        *entry.configured_name.write().unwrap() = Some(new_name);
        entry.replace_backend(new_inner);
        entry.set_level(effective_level);
        Ok(())
    }

    fn reconfigure(&'static self, new_config: RuntimeConfig) -> Result<(), String> {
        new_config.validate()?;
        let _write = self.write_lock.lock().unwrap();
        let _control = self.control_lock.lock().unwrap();

        self.flush_sink()?;

        let new_sink = Self::build_sink(&new_config, new_config.truncate)?;

        let module_entries: Vec<Arc<LoggerEntry>> = self
            .modules
            .read()
            .unwrap()
            .values()
            .flat_map(|group| group.instances.iter().cloned())
            .collect();

        let default_level = self.level_rules.read().unwrap().effective(
            DEFAULT_LOGGER_NAME,
            DEFAULT_LOGGER_NAME,
            new_config.default_level,
        );
        let new_default = Self::build_logger(DEFAULT_LOGGER_NAME, default_level, new_sink.clone())?;

        let mut rebuilt = Vec::with_capacity(module_entries.len());
        for entry in &module_entries {
            let inner = Self::build_logger(&entry.display_name(), entry.level(), new_sink.clone())?;
            rebuilt.push((entry.clone(), inner));
        }

        *self.sink.write().unwrap() = new_sink;
        self.default_logger.replace_backend(new_default);
        self.default_logger.set_level(default_level);

        for (entry, inner) in rebuilt {
            entry.replace_backend(inner);
        }

        *self.config.write().unwrap() = new_config;
        self.archive_state.lock().unwrap().bytes_since_check = 0;
        self.wake_flush_worker();
        Ok(())
    }

    fn load_level_config(&self, path: &Path) -> Result<(), String> {
        let new_rules = LevelRules::load(path)?;
        self.replace_level_rules(new_rules);
        Ok(())
    }

    fn replace_level_rules(&self, new_rules: LevelRules) {
        let _control = self.control_lock.lock().unwrap();
        let default_fallback = self.config.read().unwrap().default_level;
        let default_level = new_rules.effective(
            DEFAULT_LOGGER_NAME,
            DEFAULT_LOGGER_NAME,
            default_fallback,
        );

        self.default_logger.set_level(default_level);
        for group in self.modules.read().unwrap().values() {
            for entry in &group.instances {
                let level = new_rules.effective(
                    &entry.module_name,
                    &entry.display_name(),
                    entry.fallback_level,
                );
                entry.set_level(level);
            }
        }

        *self.level_rules.write().unwrap() = new_rules;
    }

    fn set_level_rule(&self, target: &str, level: LogLevel) -> Result<(), String> {
        let _control = self.control_lock.lock().unwrap();
        let mut rules = self.level_rules.write().unwrap();

        rules.set_config_rule(target, level)?;
        let default_fallback = self.config.read().unwrap().default_level;
        let default_level = rules.effective(
            DEFAULT_LOGGER_NAME,
            DEFAULT_LOGGER_NAME,
            default_fallback,
        );
        self.default_logger.set_level(default_level);

        for group in self.modules.read().unwrap().values() {
            for entry in &group.instances {
                let effective_level = rules.effective(
                    &entry.module_name,
                    &entry.display_name(),
                    entry.fallback_level,
                );
                entry.set_level(effective_level);
            }
        }
        Ok(())
    }

    fn set_logger_level(&self, entry: &LoggerEntry, level: LogLevel) {
        let _control = self.control_lock.lock().unwrap();
        if entry.module_name != DEFAULT_LOGGER_NAME {
            self.level_rules
                .write()
                .unwrap()
                .set_logger(entry.display_name(), level);
        }
        entry.set_level(level);
    }

    fn set_default_level(&self, level: LogLevel) {
        let _control = self.control_lock.lock().unwrap();
        self.default_logger.set_level(level);
    }

    fn set_module_level(&self, module_name: &str, level: LogLevel) -> bool {
        let _control = self.control_lock.lock().unwrap();

        if module_name == DEFAULT_LOGGER_NAME {
            self.default_logger.set_level(level);
            return true;
        }

        let module_name = sanitize_component(module_name);
        let modules = self.modules.read().unwrap();
        let Some(group) = modules.get(&module_name) else {
            return false;
        };

        self.level_rules
            .write()
            .unwrap()
            .set_module(&module_name, level);
        for entry in &group.instances {
            entry.set_level(level);
        }
        true
    }

    fn set_all_levels(&self, level: LogLevel) {
        let _control = self.control_lock.lock().unwrap();
        self.level_rules.write().unwrap().set_all(level);
        self.default_logger.set_level(level);
        for group in self.modules.read().unwrap().values() {
            for entry in &group.instances {
                entry.set_level(level);
            }
        }
    }

    fn get_logger(&self, module_name: &str) -> Option<Arc<LoggerEntry>> {
        if module_name == DEFAULT_LOGGER_NAME {
            return Some(self.default_logger.clone());
        }

        let module_name = sanitize_component(module_name);
        self.modules
            .read()
            .unwrap()
            .get(&module_name)
            .and_then(|group| group.instances.last().cloned())
    }

    fn module_loggers(&self, module_name: &str) -> Vec<Arc<LoggerEntry>> {
        if module_name == DEFAULT_LOGGER_NAME {
            return vec![self.default_logger.clone()];
        }

        let module_name = sanitize_component(module_name);
        self.modules
            .read()
            .unwrap()
            .get(&module_name)
            .map(|group| group.instances.clone())
            .unwrap_or_default()
    }

    fn module_names(&self) -> Vec<String> {
        let mut names = vec![DEFAULT_LOGGER_NAME.to_string()];
        names.extend(self.modules.read().unwrap().keys().cloned());
        names.sort();
        names
    }

    fn write(&self, entry: &LoggerEntry, level: LogLevel, function: &str, message: &str) {
        if !entry.should_log(level) {
            return;
        }

        /*
         * The lock makes the batch cut atomic: no writer can keep an old sink
         * alive while the completed family is detached for compression.
         */
        let _write = self.write_lock.lock().unwrap();
        entry.write(level, function, message);

        let estimated_bytes = function
            .len()
            .saturating_add(message.len())
            .saturating_add(128) as u64;
        if let Err(err) = self.maybe_archive_log_batch(estimated_bytes) {
            eprintln!("[hlog] automatic log archive failed: {err}");
        }
    }

    fn write_hexdump(&self, entry: &LoggerEntry, level: LogLevel, function: &str,
                     data: &[u8], format: HexdumpFormat) {
        if !entry.should_log(level) { return; }
        // One record per row supplies the same prefix to every row. Keep the
        // whole dump serialized with ordinary writers and rotation switches.
        let _write = self.write_lock.lock().unwrap();
        for row in HexdumpRows::new(data, format) {
            entry.write(level, function, &row);
            let estimated = function.len().saturating_add(row.len()).saturating_add(128) as u64;
            if let Err(err) = self.maybe_archive_log_batch(estimated) {
                eprintln!("[hlog] automatic log archive failed: {err}");
            }
        }
    }

    fn maybe_archive_log_batch(&self, estimated_bytes: u64) -> Result<(), String> {
        {
            let mut state = self.archive_state.lock().unwrap();
            state.bytes_since_check = state.bytes_since_check.saturating_add(estimated_bytes);
            if state.bytes_since_check < ARCHIVE_CHECK_INTERVAL_BYTES {
                return Ok(());
            }
            state.bytes_since_check = 0;
        }

        let config = self.config.read().unwrap().clone();
        let batch_size = config
            .rotation_size_bytes
            .checked_mul(config.max_files as u64)
            .ok_or_else(|| "log archive batch size overflow".to_string())?;
        let family = Self::log_family_paths(&config.log_path())
            .map_err(|e| format!("failed to inspect log family: {e}"))?;
        let total_size = family.iter().try_fold(0u64, |total, (_, path)| {
            let size = std::fs::metadata(path)
                .map_err(|e| format!("failed to stat '{}': {e}", path.display()))?
                .len();
            Ok::<u64, String>(total.saturating_add(size))
        })?;

        if total_size < batch_size {
            return Ok(());
        }
        self.archive_log_batch(&config, family, total_size)
    }

    fn archive_log_batch(
        &self,
        config: &RuntimeConfig,
        family: Vec<(usize, PathBuf)>,
        total_size: u64,
    ) -> Result<(), String> {
        let _control = self.control_lock.lock().unwrap();
        self.flush_sink()?;

        let base_path = config.log_path();
        let parent = base_path.parent().unwrap_or_else(|| Path::new("."));
        let base_stem = base_path
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("hlog");
        let unix_seconds = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let sequence = {
            let mut state = self.archive_state.lock().unwrap();
            state.sequence = state.sequence.saturating_add(1);
            state.sequence
        };
        let process_id = std::process::id();
        let unique = format!("{process_id}-{unix_seconds}-{sequence}");
        let staging_dir = parent.join(format!(".hlog-archive-{unique}"));
        let archive_path = parent.join(format!(
            "{base_stem}.{unix_seconds}.{process_id}.{sequence}.log.gz"
        ));

        std::fs::create_dir(&staging_dir).map_err(|e| {
            format!("failed to create staging directory '{}': {e}", staging_dir.display())
        })?;

        let mut moved = Vec::with_capacity(family.len());
        for (_, source) in family {
            let Some(file_name) = source.file_name() else {
                Self::rollback_staged_files(&moved, &staging_dir);
                return Err(format!("log path '{}' has no file name", source.display()));
            };
            let destination = staging_dir.join(file_name);
            if let Err(err) = std::fs::rename(&source, &destination) {
                Self::rollback_staged_files(&moved, &staging_dir);
                return Err(format!(
                    "failed to stage '{}' as '{}': {err}",
                    source.display(),
                    destination.display()
                ));
            }
            moved.push((source, destination));
        }

        let new_sink = match Self::build_sink(config, false) {
            Ok(sink) => sink,
            Err(err) => {
                Self::rollback_staged_files(&moved, &staging_dir);
                return Err(err);
            }
        };
        let module_entries: Vec<Arc<LoggerEntry>> = self
            .modules
            .read()
            .unwrap()
            .values()
            .flat_map(|group| group.instances.iter().cloned())
            .collect();
        let backend_result: Result<
            (Arc<Logger>, Vec<(Arc<LoggerEntry>, Arc<Logger>)>),
            String,
        > = (|| {
            let new_default = Self::build_logger(
                DEFAULT_LOGGER_NAME,
                self.default_logger.level(),
                new_sink.clone(),
            )?;
            let mut rebuilt = Vec::with_capacity(module_entries.len());
            for entry in &module_entries {
                let inner =
                    Self::build_logger(&entry.display_name(), entry.level(), new_sink.clone())?;
                rebuilt.push((entry.clone(), inner));
            }
            Ok((new_default, rebuilt))
        })();
        let (new_default, rebuilt) = match backend_result {
            Ok(backends) => backends,
            Err(err) => {
                drop(new_sink);
                let _ = Self::clear_log_family(&base_path);
                Self::rollback_staged_files(&moved, &staging_dir);
                return Err(err);
            }
        };

        *self.sink.write().unwrap() = new_sink;
        self.default_logger.replace_backend(new_default);
        for (entry, inner) in rebuilt {
            entry.replace_backend(inner);
        }

        let staged_paths = moved.into_iter().map(|(_, destination)| destination).collect();
        self.start_archive_worker(staging_dir, staged_paths, archive_path.clone())?;
        eprintln!(
            "[hlog] detached {} bytes for archive '{}'; continuing in '{}'",
            total_size,
            archive_path.display(),
            base_path.display()
        );
        Ok(())
    }

    fn rollback_staged_files(moved: &[(PathBuf, PathBuf)], staging_dir: &Path) {
        for (source, destination) in moved.iter().rev() {
            let _ = std::fs::rename(destination, source);
        }
        let _ = std::fs::remove_dir(staging_dir);
    }

    fn start_archive_worker(
        &self,
        staging_dir: PathBuf,
        staged_paths: Vec<PathBuf>,
        archive_path: PathBuf,
    ) -> Result<(), String> {
        let mut slot = self.archive_worker.lock().unwrap();
        if slot.is_none() {
            let (tx, rx) = mpsc::channel::<ArchiveCommand>();
            let handle = thread::Builder::new()
                .name("hlog-archive".to_string())
                .spawn(move || {
                    while let Ok(command) = rx.recv() {
                        match command {
                            ArchiveCommand::Compress(job) => Self::compress_archive_job(job),
                            ArchiveCommand::Stop => break,
                        }
                    }
                })
                .map_err(|e| format!("failed to start log archive worker: {e}"))?;
            *slot = Some(ArchiveWorker { tx, handle });
        }

        let job = ArchiveJob {
            staging_dir,
            staged_paths,
            archive_path,
        };
        slot.as_ref()
            .unwrap()
            .tx
            .send(ArchiveCommand::Compress(job))
            .map_err(|e| format!("failed to queue log archive: {e}"))
    }

    fn compress_archive_job(job: ArchiveJob) {
        let result = (|| -> Result<(), String> {
            let output = File::create(&job.archive_path).map_err(|e| {
                format!(
                    "failed to create archive '{}': {e}",
                    job.archive_path.display()
                )
            })?;
            let status = Command::new("gzip")
                .arg("-c")
                .args(&job.staged_paths)
                .stdin(Stdio::null())
                .stdout(Stdio::from(output))
                .status()
                .map_err(|e| format!("failed to execute gzip: {e}"))?;

            if !status.success() {
                return Err(format!("gzip exited with status {status}"));
            }
            Ok(())
        })();

        match result {
            Ok(()) => {
                let mut cleanup_failed = false;
                for path in &job.staged_paths {
                    if let Err(err) = std::fs::remove_file(path) {
                        cleanup_failed = true;
                        eprintln!(
                            "[hlog] archive '{}' is valid, but failed to remove staged log '{}': {err}",
                            job.archive_path.display(),
                            path.display()
                        );
                    }
                }
                if !cleanup_failed {
                    if let Err(err) = std::fs::remove_dir(&job.staging_dir) {
                        eprintln!(
                            "[hlog] archive '{}' is valid, but failed to remove staging directory '{}': {err}",
                            job.archive_path.display(),
                            job.staging_dir.display()
                        );
                    }
                }
                eprintln!(
                    "[hlog] compressed log archive '{}'",
                    job.archive_path.display()
                );
            }
            Err(err) => {
                let _ = std::fs::remove_file(&job.archive_path);
                eprintln!(
                    "[hlog] log compression failed: {err}; uncompressed logs remain in '{}'",
                    job.staging_dir.display()
                );
            }
        }
    }

    fn flush_sink(&self) -> Result<(), String> {
        self.sink
            .read()
            .unwrap()
            .flush()
            .map_err(|e| format!("failed to flush log sink: {e}"))
    }

    fn flush(&self) -> Result<(), String> {
        let _write = self.write_lock.lock().unwrap();
        self.flush_sink()
    }

    fn set_flush_interval(&'static self, interval_ms: u32) -> Result<(), String> {
        self.config.write().unwrap().flush_interval_ms = interval_ms;
        self.start_flush_worker()?;
        self.wake_flush_worker();
        Ok(())
    }

    fn start_flush_worker(&'static self) -> Result<(), String> {
        let mut slot = self.flush_worker.lock().unwrap();
        if slot.is_some() {
            return Ok(());
        }

        let (tx, rx) = mpsc::channel::<FlushCommand>();
        let center: &'static LogCenter = self;

        let handle = thread::Builder::new()
            .name("hlog-flush".to_string())
            .spawn(move || loop {
                let interval_ms = center.config.read().unwrap().flush_interval_ms;

                if interval_ms == 0 {
                    match rx.recv() {
                        Ok(FlushCommand::Wake) => continue,
                        Ok(FlushCommand::Stop) | Err(_) => {
                            let _ = center.flush();
                            break;
                        }
                    }
                } else {
                    match rx.recv_timeout(Duration::from_millis(interval_ms as u64)) {
                        Err(RecvTimeoutError::Timeout) => {
                            let _ = center.flush();
                        }
                        Ok(FlushCommand::Wake) => continue,
                        Ok(FlushCommand::Stop) | Err(RecvTimeoutError::Disconnected) => {
                            let _ = center.flush();
                            break;
                        }
                    }
                }
            })
            .map_err(|e| format!("failed to start flush worker: {e}"))?;

        *slot = Some(FlushWorker { tx, handle });
        Ok(())
    }

    fn wake_flush_worker(&self) {
        if let Some(worker) = self.flush_worker.lock().unwrap().as_ref() {
            let _ = worker.tx.send(FlushCommand::Wake);
        }
    }

    fn shutdown(&self) {
        let worker = self.flush_worker.lock().unwrap().take();
        if let Some(worker) = worker {
            let _ = worker.tx.send(FlushCommand::Stop);
            let _ = worker.handle.join();
        }
        let _ = self.flush();

        let archive_worker = self.archive_worker.lock().unwrap().take();
        if let Some(worker) = archive_worker {
            let _ = worker.tx.send(ArchiveCommand::Stop);
            let _ = worker.handle.join();
        }
    }

    fn log_family_paths(base: &Path) -> std::io::Result<Vec<(usize, PathBuf)>> {
        let Some(parent) = base.parent() else {
            return Ok(Vec::new());
        };
        if !parent.exists() {
            return Ok(Vec::new());
        }

        let Some(base_stem) = base.file_stem().and_then(|s| s.to_str()) else {
            return Ok(Vec::new());
        };
        let extension = base.extension().and_then(|s| s.to_str());
        let prefix = format!("{base_stem}_");
        let mut paths = Vec::new();

        for item in std::fs::read_dir(parent)? {
            let item = item?;
            if !item.file_type()?.is_file() {
                continue;
            }

            let path = item.path();
            if path.extension().and_then(|s| s.to_str()) != extension {
                continue;
            }

            let Some(stem) = path.file_stem().and_then(|s| s.to_str()) else {
                continue;
            };
            if stem == base_stem {
                paths.push((0, path));
                continue;
            }
            let Some(index) = stem.strip_prefix(&prefix) else {
                continue;
            };

            if !index.is_empty() && index.bytes().all(|b| b.is_ascii_digit()) {
                if let Ok(index) = index.parse::<usize>() {
                    paths.push((index, path));
                }
            }
        }

        /* Rotated suffixes increase with age, so gzip oldest to newest. */
        paths.sort_by(|left, right| right.0.cmp(&left.0));
        Ok(paths)
    }

    fn clear_log_family(base: &Path) -> std::io::Result<()> {
        for (_, path) in Self::log_family_paths(base)? {
            std::fs::remove_file(path)?;
        }
        Ok(())
    }
}

fn ensure_center() -> Result<&'static LogCenter, String> {
    if let Some(center) = LOG_CENTER.get() {
        center.start_flush_worker()?;
        return Ok(center);
    }

    let candidate = LogCenter::new(RuntimeConfig::default())?;
    let _ = LOG_CENTER.set(candidate);

    let center = LOG_CENTER
        .get()
        .ok_or_else(|| "failed to install global LogCenter".to_string())?;
    center.start_flush_worker()?;
    Ok(center)
}

fn set_last_error(message: impl AsRef<str>) {
    let mut value = message.as_ref().replace('\0', "\\0");
    if value.is_empty() {
        value.push_str("unknown error");
    }
    let cstr = CString::new(value).unwrap_or_else(|_| CString::new("invalid error string").unwrap());
    LAST_ERROR.with(|slot| *slot.borrow_mut() = cstr);
}

fn clear_last_error() {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = CString::new("").unwrap());
}

fn current_last_error() -> Option<String> {
    LAST_ERROR.with(|slot| slot.borrow().to_str().ok().map(str::to_string))
}

fn ffi_guard<T: Copy>(fallback: T, f: impl FnOnce() -> Result<T, (i32, String)>) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(Ok(value)) => {
            clear_last_error();
            value
        }
        Ok(Err((_code, message))) => {
            set_last_error(message);
            fallback
        }
        Err(_) => {
            set_last_error("panic crossed hlog FFI boundary");
            fallback
        }
    }
}

fn ffi_guard_status(f: impl FnOnce() -> Result<(), (i32, String)>) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(Ok(())) => {
            clear_last_error();
            HLOG_OK
        }
        Ok(Err((code, message))) => {
            set_last_error(message);
            code
        }
        Err(_) => {
            set_last_error("panic crossed hlog FFI boundary");
            HLOG_ERR_INTERNAL
        }
    }
}

unsafe fn required_cstr(ptr: *const std::os::raw::c_char, field: &str) -> Result<String, (i32, String)> {
    if ptr.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            format!("{field} must not be NULL"),
        ));
    }
    CStr::from_ptr(ptr)
        .to_str()
        .map(str::to_string)
        .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, format!("{field} is not valid UTF-8: {e}")))
}

unsafe fn optional_cstr(ptr: *const std::os::raw::c_char) -> Result<Option<String>, (i32, String)> {
    if ptr.is_null() {
        return Ok(None);
    }
    let value = CStr::from_ptr(ptr)
        .to_str()
        .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, format!("string is not valid UTF-8: {e}")))?;
    if value.is_empty() {
        Ok(None)
    } else {
        Ok(Some(value.to_string()))
    }
}

unsafe fn bytes_arg<'a>(
    ptr: *const u8,
    len: usize,
    field: &str,
) -> Result<&'a [u8], (i32, String)> {
    if len == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            format!("{field} must not be NULL when len != 0"),
        ));
    }
    Ok(slice::from_raw_parts(ptr, len))
}

fn sanitize_component(name: &str) -> String {
    let value: String = name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.' {
                c
            } else {
                '_'
            }
        })
        .collect();

    let value = value.trim_matches('_').to_string();
    if value.is_empty() {
        "unnamed".to_string()
    } else {
        value
    }
}

fn entry_to_handle(entry: &Arc<LoggerEntry>) -> *mut HLogLogger {
    Arc::as_ptr(entry) as *mut HLogLogger
}

unsafe fn entry_from_handle<'a>(handle: *const HLogLogger) -> Result<&'a LoggerEntry, (i32, String)> {
    if handle.is_null() {
        return Err((HLOG_ERR_INVALID_ARGUMENT, "logger handle is NULL".to_string()));
    }
    Ok(&*(handle as *const LoggerEntry))
}

fn copy_string_to_buffer(value: &str, buffer: *mut std::os::raw::c_char, capacity: usize) -> usize {
    let bytes = value.as_bytes();
    let required = bytes.len() + 1;

    if buffer.is_null() || capacity == 0 {
        return required;
    }

    let copy_len = bytes.len().min(capacity.saturating_sub(1));
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buffer as *mut u8, copy_len);
        *buffer.add(copy_len) = 0;
    }
    required
}

unsafe fn query_params_from_ffi(
    params: *const HLogQueryParamSpec,
    param_count: usize,
) -> Result<Vec<QueryParamSpec>, (i32, String)> {
    if param_count > HLOG_QUERY_MAX_PARAMS {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            format!("query declares {param_count} parameters; maximum is {HLOG_QUERY_MAX_PARAMS}"),
        ));
    }
    if param_count != 0 && params.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            "query params must not be NULL when param_count is nonzero".to_string(),
        ));
    }

    let raw_params = if param_count == 0 {
        &[][..]
    } else {
        slice::from_raw_parts(params, param_count)
    };
    let mut result = Vec::with_capacity(param_count);
    let mut names = std::collections::HashSet::new();

    for (index, raw) in raw_params.iter().enumerate() {
        let name = required_cstr(raw.name, &format!("params[{index}].name"))?;
        if !valid_rule_component(&name) {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                format!("invalid query parameter name '{name}'"),
            ));
        }
        if !names.insert(name.clone()) {
            return Err((
                HLOG_ERR_DUPLICATE,
                format!("duplicate query parameter '{name}'"),
            ));
        }
        if raw.flags & !HLOG_QUERY_PARAM_VALID_FLAGS != 0 {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                format!("query parameter '{name}' has invalid flags {:#x}", raw.flags),
            ));
        }

        result.push(QueryParamSpec {
            name,
            param_type: QueryParamType::from_ffi(raw.param_type)
                .map_err(|message| (HLOG_ERR_INVALID_ARGUMENT, message))?,
            required: raw.flags & HLOG_QUERY_PARAM_REQUIRED != 0,
            repeatable: raw.flags & HLOG_QUERY_PARAM_REPEATABLE != 0,
            description: optional_cstr(raw.description)?.unwrap_or_default(),
        });
    }
    Ok(result)
}

unsafe fn query_request_args<'a>(
    request: *const HLogQueryRequest,
) -> Result<&'a [HLogQueryArg], (i32, String)> {
    if request.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            "query request must not be NULL".to_string(),
        ));
    }
    let request = &*request;
    if request.arg_count == 0 {
        return Ok(&[]);
    }
    if request.args.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            "query request args must not be NULL when arg_count is nonzero".to_string(),
        ));
    }
    Ok(slice::from_raw_parts(request.args, request.arg_count))
}

unsafe fn query_arg_value(
    request: *const HLogQueryRequest,
    name: &str,
    occurrence: usize,
) -> Result<Option<*const std::os::raw::c_char>, (i32, String)> {
    let mut matched = 0usize;
    for arg in query_request_args(request)? {
        if arg.name.is_null() || arg.value.is_null() {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                "query request contains a NULL argument".to_string(),
            ));
        }
        if CStr::from_ptr(arg.name).to_bytes() == name.as_bytes() {
            if matched == occurrence {
                return Ok(Some(arg.value));
            }
            matched += 1;
        }
    }
    Ok(None)
}

unsafe fn config_from_ffi(config: *const HLogConfig) -> Result<RuntimeConfig, (i32, String)> {
    if config.is_null() {
        return Err((HLOG_ERR_INVALID_ARGUMENT, "config must not be NULL".to_string()));
    }

    let config = &*config;
    if config.struct_size < std::mem::size_of::<HLogConfig>() as u32 {
        return Err((
            HLOG_ERR_ABI_MISMATCH,
            format!(
                "HLogConfig.struct_size={} is smaller than runtime size {}",
                config.struct_size,
                std::mem::size_of::<HLogConfig>()
            ),
        ));
    }
    if config.abi_version != HLOG_ABI_VERSION {
        return Err((
            HLOG_ERR_ABI_MISMATCH,
            format!(
                "ABI mismatch: caller={}, runtime={}",
                config.abi_version, HLOG_ABI_VERSION
            ),
        ));
    }

    let base_dir = required_cstr(config.base_dir, "base_dir")?;
    let file_name = required_cstr(config.file_name, "file_name")?;
    let default_level = LogLevel::from_ffi(config.default_level)
        .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;

    let runtime = RuntimeConfig {
        base_dir: PathBuf::from(base_dir),
        file_name,
        default_level,
        truncate: config.truncate != 0,
        rotation_size_bytes: config.rotation_size_bytes,
        max_files: config.max_files,
        flush_interval_ms: config.flush_interval_ms,
    };
    runtime
        .validate()
        .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
    Ok(runtime)
}

#[no_mangle]
pub extern "C" fn hlog_abi_version() -> u32 {
    HLOG_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn hlog_last_error() -> *const std::os::raw::c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn hlog_init_default() -> i32 {
    ffi_guard_status(|| ensure_center().map(|_| ()).map_err(|e| (HLOG_ERR_INTERNAL, e)))
}

#[no_mangle]
pub extern "C" fn hlog_init(config: *const HLogConfig) -> i32 {
    ffi_guard_status(|| {
        let config = unsafe { config_from_ffi(config)? };
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center
            .reconfigure(config)
            .map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center
            .start_flush_worker()
            .map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_load_level_config(path: *const std::os::raw::c_char) -> i32 {
    ffi_guard_status(|| {
        let path = unsafe { required_cstr(path, "path")? };
        if path.trim().is_empty() {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                "level config path must not be empty".to_string(),
            ));
        }

        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.load_level_config(Path::new(&path)).map_err(|e| {
            let code = if e.contains("failed to read hlog level config") {
                HLOG_ERR_NOT_FOUND
            } else {
                HLOG_ERR_INVALID_ARGUMENT
            };
            (code, e)
        })?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_set_level_rule(
    target: *const std::os::raw::c_char,
    level: u32,
) -> i32 {
    ffi_guard_status(|| {
        let target = unsafe { required_cstr(target, "target")? };
        let level = LogLevel::from_ffi(level).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;

        center
            .set_level_rule(&target, level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_register(
    module: *const std::os::raw::c_char,
    instance: *const std::os::raw::c_char,
    name: *const std::os::raw::c_char,
    description: *const std::os::raw::c_char,
    params: *const HLogQueryParamSpec,
    param_count: usize,
    callback: Option<HLogQueryCallback>,
    opaque: *mut c_void,
) -> u64 {
    ffi_guard(0, || {
        let module = unsafe { required_cstr(module, "module")? };
        let instance = unsafe { required_cstr(instance, "instance")? };
        let name = unsafe { required_cstr(name, "name")? };
        let description = unsafe { optional_cstr(description)? }.unwrap_or_default();
        let params = unsafe { query_params_from_ffi(params, param_count)? };
        let callback = callback.ok_or_else(|| {
            (
                HLOG_ERR_INVALID_ARGUMENT,
                "query callback must not be NULL".to_string(),
            )
        })?;
        let center = ensure_center().map_err(|message| (HLOG_ERR_INTERNAL, message))?;
        center
            .queries
            .register(module, instance, name, description, params, callback, opaque)
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_unregister(handle: u64) -> i32 {
    ffi_guard_status(|| {
        let center = ensure_center().map_err(|message| (HLOG_ERR_INTERNAL, message))?;
        center.queries.unregister(handle)
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_count() -> usize {
    ffi_guard(0, || {
        let center = ensure_center().map_err(|message| (HLOG_ERR_INTERNAL, message))?;
        Ok(center.queries.entries().len())
    })
}

fn query_entry_at(index: usize) -> Result<Arc<QueryEntry>, (i32, String)> {
    let center = ensure_center().map_err(|message| (HLOG_ERR_INTERNAL, message))?;
    center.queries.entries().get(index).cloned().ok_or_else(|| {
        (
            HLOG_ERR_NOT_FOUND,
            format!("query index {index} is out of range"),
        )
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_target_at(
    index: usize,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0, || {
        let entry = query_entry_at(index)?;
        Ok(copy_string_to_buffer(&entry.target, buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_description_at(
    index: usize,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0, || {
        let entry = query_entry_at(index)?;
        Ok(copy_string_to_buffer(&entry.description, buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_usage_at(
    index: usize,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0, || {
        let entry = query_entry_at(index)?;
        Ok(copy_string_to_buffer(&entry.usage(), buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_execute(
    target: *const std::os::raw::c_char,
    args: *const std::os::raw::c_char,
    write: Option<HLogQueryWriteFn>,
    write_opaque: *mut c_void,
) -> i32 {
    ffi_guard_status(|| {
        let target = unsafe { required_cstr(target, "target")? };
        let args = unsafe { optional_cstr(args)? }.unwrap_or_default();
        let write = write.ok_or_else(|| {
            (
                HLOG_ERR_INVALID_ARGUMENT,
                "query output writer must not be NULL".to_string(),
            )
        })?;
        let center = ensure_center().map_err(|message| (HLOG_ERR_INTERNAL, message))?;
        center.queries.execute(&target, &args, write, write_opaque)
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_count(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
) -> usize {
    ffi_guard(0, || {
        let name = unsafe { required_cstr(name, "name")? };
        let args = unsafe { query_request_args(request)? };
        let mut count = 0usize;
        for arg in args {
            if arg.name.is_null() {
                return Err((
                    HLOG_ERR_INVALID_ARGUMENT,
                    "query request contains a NULL argument name".to_string(),
                ));
            }
            if unsafe { CStr::from_ptr(arg.name).to_bytes() } == name.as_bytes() {
                count += 1;
            }
        }
        Ok(count)
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_at(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    occurrence: usize,
) -> *const std::os::raw::c_char {
    ffi_guard(ptr::null(), || {
        let name = unsafe { required_cstr(name, "name")? };
        unsafe { query_arg_value(request, &name, occurrence) }
            .and_then(|value| value.ok_or_else(|| {
                (
                    HLOG_ERR_NOT_FOUND,
                    format!("query parameter '{name}' occurrence {occurrence} was not provided"),
                )
            }))
    })
}

fn parse_query_arg_value<T: str::FromStr>(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    output: *mut T,
    type_name: &str,
) -> Result<(), (i32, String)> {
    if output.is_null() {
        return Err((
            HLOG_ERR_INVALID_ARGUMENT,
            "query argument output must not be NULL".to_string(),
        ));
    }
    let name = unsafe { required_cstr(name, "name")? };
    let value = unsafe { query_arg_value(request, &name, 0)? }.ok_or_else(|| {
        (
            HLOG_ERR_NOT_FOUND,
            format!("required query parameter '{name}' was not provided"),
        )
    })?;
    let text = unsafe { CStr::from_ptr(value) }
        .to_str()
        .map_err(|error| (HLOG_ERR_INVALID_ARGUMENT, format!("parameter '{name}' is not UTF-8: {error}")))?;
    let parsed = text.parse::<T>().map_err(|_| {
        (
            HLOG_ERR_INVALID_ARGUMENT,
            format!("invalid {type_name} value '{text}' for parameter '{name}'"),
        )
    })?;
    unsafe { *output = parsed };
    Ok(())
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_u32(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    value: *mut u32,
) -> i32 {
    ffi_guard_status(|| parse_query_arg_value(request, name, value, "u32"))
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_u64(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    value: *mut u64,
) -> i32 {
    ffi_guard_status(|| parse_query_arg_value(request, name, value, "u64"))
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_i64(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    value: *mut i64,
) -> i32 {
    ffi_guard_status(|| parse_query_arg_value(request, name, value, "i64"))
}

#[no_mangle]
pub extern "C" fn hlog_query_arg_bool(
    request: *const HLogQueryRequest,
    name: *const std::os::raw::c_char,
    value: *mut bool,
) -> i32 {
    ffi_guard_status(|| {
        if value.is_null() {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                "query argument output must not be NULL".to_string(),
            ));
        }
        let name = unsafe { required_cstr(name, "name")? };
        let raw = unsafe { query_arg_value(request, &name, 0)? }.ok_or_else(|| {
            (
                HLOG_ERR_NOT_FOUND,
                format!("required query parameter '{name}' was not provided"),
            )
        })?;
        let text = unsafe { CStr::from_ptr(raw) }
            .to_str()
            .map_err(|error| (HLOG_ERR_INVALID_ARGUMENT, format!("parameter '{name}' is not UTF-8: {error}")))?;
        let parsed = if text == "1" || text.eq_ignore_ascii_case("true") || text.eq_ignore_ascii_case("yes") {
            true
        } else if text == "0" || text.eq_ignore_ascii_case("false") || text.eq_ignore_ascii_case("no") {
            false
        } else {
            return Err((
                HLOG_ERR_INVALID_ARGUMENT,
                format!("invalid bool value '{text}' for parameter '{name}'"),
            ));
        };
        unsafe { *value = parsed };
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_query_set_error(
    code: i32,
    message: *const std::os::raw::c_char,
) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
        required_cstr(message, "message")
    })) {
        Ok(Ok(message)) => {
            set_last_error(message);
            if code == HLOG_OK { HLOG_ERR_INTERNAL } else { code }
        }
        Ok(Err((_code, message))) => {
            set_last_error(message);
            HLOG_ERR_INVALID_ARGUMENT
        }
        Err(_) => {
            set_last_error("panic crossed hlog query error boundary");
            HLOG_ERR_INTERNAL
        }
    }
}

#[no_mangle]
pub extern "C" fn hlog_register_logger(
    module_name: *const std::os::raw::c_char,
    configured_name: *const std::os::raw::c_char,
    level: u32,
) -> *mut HLogLogger {
    ffi_guard(ptr::null_mut(), || {
        let module_name = unsafe { required_cstr(module_name, "module_name")? };
        let configured_name = unsafe { optional_cstr(configured_name)? };
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let entry = center
            .register_logger(&module_name, configured_name.as_deref(), level)
            .map_err(|e| {
                let code = if e.contains("duplicate logger name") {
                    HLOG_ERR_DUPLICATE
                } else {
                    HLOG_ERR_INTERNAL
                };
                (code, e)
            })?;
        Ok(entry_to_handle(&entry))
    })
}

#[no_mangle]
pub extern "C" fn hlog_default_logger() -> *mut HLogLogger {
    ffi_guard(ptr::null_mut(), || {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(entry_to_handle(&center.default_logger))
    })
}

#[no_mangle]
pub extern "C" fn hlog_get_logger(
    module_name: *const std::os::raw::c_char,
) -> *mut HLogLogger {
    ffi_guard(ptr::null_mut(), || {
        let module_name = unsafe { required_cstr(module_name, "module_name")? };
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let entry = center.get_logger(&module_name).ok_or_else(|| {
            (
                HLOG_ERR_NOT_FOUND,
                format!("module '{module_name}' has no logger"),
            )
        })?;
        Ok(entry_to_handle(&entry))
    })
}

#[no_mangle]
pub extern "C" fn hlog_module_logger_count(
    module_name: *const std::os::raw::c_char,
) -> usize {
    ffi_guard(0usize, || {
        let module_name = unsafe { required_cstr(module_name, "module_name")? };
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(center.module_loggers(&module_name).len())
    })
}

#[no_mangle]
pub extern "C" fn hlog_module_logger_at(
    module_name: *const std::os::raw::c_char,
    index: usize,
) -> *mut HLogLogger {
    ffi_guard(ptr::null_mut(), || {
        let module_name = unsafe { required_cstr(module_name, "module_name")? };
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let entries = center.module_loggers(&module_name);
        let entry = entries.get(index).ok_or_else(|| {
            (
                HLOG_ERR_NOT_FOUND,
                format!("logger index {index} out of range for module '{module_name}'"),
            )
        })?;
        Ok(entry_to_handle(entry))
    })
}

#[no_mangle]
pub extern "C" fn hlog_module_count() -> usize {
    ffi_guard(0usize, || {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(center.module_names().len())
    })
}

#[no_mangle]
pub extern "C" fn hlog_module_name_at(
    index: usize,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0usize, || {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let names = center.module_names();
        let value = names.get(index).ok_or_else(|| {
            (
                HLOG_ERR_NOT_FOUND,
                format!("module index {index} out of range"),
            )
        })?;
        Ok(copy_string_to_buffer(value, buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_module_name(
    handle: *const HLogLogger,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0usize, || {
        let entry = unsafe { entry_from_handle(handle)? };
        Ok(copy_string_to_buffer(&entry.module_name, buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_configured_name(
    handle: *const HLogLogger,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0usize, || {
        let entry = unsafe { entry_from_handle(handle)? };
        let Some(name) = entry.configured_name() else {
            return Ok(0usize);
        };
        Ok(copy_string_to_buffer(&name, buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_display_name(
    handle: *const HLogLogger,
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0usize, || {
        let entry = unsafe { entry_from_handle(handle)? };
        Ok(copy_string_to_buffer(&entry.display_name(), buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_instance_id(handle: *const HLogLogger) -> usize {
    ffi_guard(0usize, || {
        let entry = unsafe { entry_from_handle(handle)? };
        Ok(entry.instance_id)
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_level(handle: *const HLogLogger) -> u32 {
    ffi_guard(LogLevel::Off as u32, || {
        let entry = unsafe { entry_from_handle(handle)? };
        Ok(entry.level() as u32)
    })
}

#[no_mangle]
pub extern "C" fn hlog_enabled(handle: *const HLogLogger, level: u32) -> bool {
    ffi_guard(false, || {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        Ok(entry.should_log(level))
    })
}

#[no_mangle]
pub extern "C" fn hlog_write(
    handle: *const HLogLogger,
    level: u32,
    function: *const u8,
    function_len: usize,
    _file: *const u8,
    _file_len: usize,
    _line: u32,
    message: *const u8,
    message_len: usize,
) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;

        if !entry.should_log(level) {
            return Ok(());
        }

        let function = unsafe { bytes_arg(function, function_len, "function")? };
        let message = unsafe { bytes_arg(message, message_len, "message")? };
        let function = String::from_utf8_lossy(function);
        let message = String::from_utf8_lossy(message);
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.write(entry, level, &function, &message);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_hexdump(
    handle: *const HLogLogger,
    level: u32,
    function: *const u8,
    function_len: usize,
    _file: *const u8,
    _file_len: usize,
    _line: u32,
    data: *const u8,
    data_len: usize,
) -> i32 {
    hlog_hexdump_ex(handle, level, function, function_len, _file, _file_len, _line, data, data_len, 1)
}

#[no_mangle]
pub extern "C" fn hlog_hexdump_ex(
    handle: *const HLogLogger, level: u32,
    function: *const u8, function_len: usize,
    _file: *const u8, _file_len: usize, _line: u32,
    data: *const u8, data_len: usize, unit_bytes: u32,
) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        if !entry.should_log(level) {
            return Ok(());
        }

        let format = HexdumpFormat::from_ffi(unit_bytes).map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let function = unsafe { bytes_arg(function, function_len, "function")? };
        let data = unsafe { bytes_arg(data, data_len, "data")? };
        let function = String::from_utf8_lossy(function);
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.write_hexdump(entry, level, &function, data, format);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_set_level(handle: *mut HLogLogger, level: u32) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.set_logger_level(entry, level);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_logger_config_name(
    handle: *mut HLogLogger,
    new_name: *const std::os::raw::c_char,
) -> i32 {
    ffi_guard_status(|| {
        let entry = unsafe { entry_from_handle(handle)? };
        let new_name = unsafe { required_cstr(new_name, "new_name")? };
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.config_logger_name(entry, &new_name).map_err(|e| {
            let code = if e.contains("duplicate logger name") {
                HLOG_ERR_DUPLICATE
            } else {
                HLOG_ERR_INTERNAL
            };
            (code, e)
        })?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_set_default_level(level: u32) -> i32 {
    ffi_guard_status(|| {
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.set_default_level(level);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_set_module_level(
    module_name: *const std::os::raw::c_char,
    level: u32,
) -> i32 {
    ffi_guard_status(|| {
        let module_name = unsafe { required_cstr(module_name, "module_name")? };
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        if !center.set_module_level(&module_name, level) {
            return Err((
                HLOG_ERR_NOT_FOUND,
                format!("module '{module_name}' not found"),
            ));
        }
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_set_all_levels(level: u32) -> i32 {
    ffi_guard_status(|| {
        let level = LogLevel::from_ffi(level)
            .map_err(|e| (HLOG_ERR_INVALID_ARGUMENT, e))?;
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.set_all_levels(level);
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_flush() -> i32 {
    ffi_guard_status(|| {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center.flush().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_set_flush_interval_ms(interval_ms: u32) -> i32 {
    ffi_guard_status(|| {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        center
            .set_flush_interval(interval_ms)
            .map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        Ok(())
    })
}

#[no_mangle]
pub extern "C" fn hlog_file_path(
    buffer: *mut std::os::raw::c_char,
    capacity: usize,
) -> usize {
    ffi_guard(0usize, || {
        let center = ensure_center().map_err(|e| (HLOG_ERR_INTERNAL, e))?;
        let path = center.config.read().unwrap().log_path();
        Ok(copy_string_to_buffer(&path.to_string_lossy(), buffer, capacity))
    })
}

#[no_mangle]
pub extern "C" fn hlog_shutdown() {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(center) = LOG_CENTER.get() {
            center.shutdown();
        }
    }));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn query_param(name: &str, param_type: QueryParamType, required: bool) -> QueryParamSpec {
        QueryParamSpec {
            name: name.to_string(),
            param_type,
            required,
            repeatable: false,
            description: String::new(),
        }
    }

    #[test]
    fn query_arguments_are_dynamic_named_and_typed() {
        let specs = vec![
            query_param("vf", QueryParamType::U32, true),
            query_param("l_pool", QueryParamType::U32, false),
            query_param("detail", QueryParamType::Bool, false),
            query_param("label", QueryParamType::String, false),
        ];

        let parsed = parse_query_args("vf=3 detail=true label=\"VF three\"", &specs).unwrap();
        assert_eq!(parsed.names.len(), 3);
        assert_eq!(parsed.values[0].to_str().unwrap(), "3");
        assert_eq!(parsed.values[2].to_str().unwrap(), "VF three");

        assert!(parse_query_args("detail=true", &specs).unwrap_err().contains("missing required"));
        assert!(parse_query_args("vf=bad", &specs).unwrap_err().contains("expected u32"));
        assert!(parse_query_args("vf=3 unknown=1", &specs).unwrap_err().contains("unknown"));
        assert!(parse_query_args("vf=3 vf=4", &specs).unwrap_err().contains("only be specified once"));
    }

    #[test]
    fn level_rules_apply_exact_module_all_precedence() {
        let rules = LevelRules::parse(
            "bmu/1 = trace\nbmu = info\nall = warn\ncrg = off\n",
            Path::new("levels.conf"),
        )
        .unwrap();

        assert_eq!(
            rules.effective("bmu", "bmu/1", LogLevel::Error),
            LogLevel::Trace
        );
        assert_eq!(
            rules.effective("bmu", "bmu/2", LogLevel::Error),
            LogLevel::Info
        );
        assert_eq!(
            rules.effective("crg", "crg/1", LogLevel::Error),
            LogLevel::Off
        );
        assert_eq!(
            rules.effective("its", "its/1", LogLevel::Error),
            LogLevel::Warn
        );
    }

    #[test]
    fn dynamic_module_and_all_updates_replace_narrower_rules() {
        let mut rules = LevelRules::parse(
            "all = warn\nbmu = info\nbmu/2 = trace\n",
            Path::new("levels.conf"),
        )
        .unwrap();

        rules.set_module("bmu", LogLevel::Debug);
        assert_eq!(
            rules.effective("bmu", "bmu/2", LogLevel::Error),
            LogLevel::Debug
        );

        rules.set_all(LogLevel::Critical);
        assert_eq!(
            rules.effective("bmu", "bmu/2", LogLevel::Error),
            LogLevel::Critical
        );
    }

    #[test]
    fn structured_config_rules_preserve_narrower_overrides() {
        let mut rules = LevelRules::default();

        rules.set_config_rule("bmu/2", LogLevel::Debug).unwrap();
        rules.set_config_rule("bmu", LogLevel::Info).unwrap();
        rules.set_config_rule("all", LogLevel::Warn).unwrap();

        assert_eq!(
            rules.effective("bmu", "bmu/1", LogLevel::Error),
            LogLevel::Info
        );
        assert_eq!(
            rules.effective("bmu", "bmu/2", LogLevel::Error),
            LogLevel::Debug
        );
        assert_eq!(
            rules.effective("crg", "crg/1", LogLevel::Error),
            LogLevel::Warn
        );
    }

    #[test]
    fn malformed_and_duplicate_rules_are_rejected() {
        let path = Path::new("levels.conf");

        assert!(LevelRules::parse("bmu debug\n", path).is_err());
        assert!(LevelRules::parse("bmu = verbose\n", path).is_err());
        assert!(LevelRules::parse("bmu = info\nbmu = debug\n", path).is_err());
        assert!(LevelRules::parse("bmu//1 = debug\n", path).is_err());
    }

    #[test]
    fn log_family_is_oldest_first_and_clear_preserves_archives() {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "hlog-family-test-{}-{unique}",
            std::process::id()
        ));
        std::fs::create_dir(&directory).unwrap();
        let base = directory.join("qemu_device.log");
        let rotated_one = directory.join("qemu_device_1.log");
        let rotated_two = directory.join("qemu_device_2.log");
        let archive = directory.join("qemu_device.1.1.log.gz");
        std::fs::write(&base, b"new\n").unwrap();
        std::fs::write(&rotated_one, b"middle\n").unwrap();
        std::fs::write(&rotated_two, b"old\n").unwrap();
        std::fs::write(&archive, b"archive\n").unwrap();

        let family = LogCenter::log_family_paths(&base).unwrap();
        let names: Vec<_> = family
            .iter()
            .map(|(_, path)| path.file_name().unwrap().to_string_lossy().to_string())
            .collect();
        assert_eq!(
            names,
            vec!["qemu_device_2.log", "qemu_device_1.log", "qemu_device.log"]
        );

        LogCenter::clear_log_family(&base).unwrap();
        assert!(archive.exists());
        assert!(!base.exists());
        assert!(!rotated_one.exists());
        assert!(!rotated_two.exists());
        std::fs::remove_file(archive).unwrap();
        std::fs::remove_dir(directory).unwrap();
    }
}
