# hlog

`hlog` is a process-global logging runtime for QEMU and dynamically loaded C, C++, and Rust device models. Every
frontend writes through one `libhlog.so.1`, one rotating file sink, one log file, and one flush worker.
Console mirroring is optional and off by default; explicit console-only macros are also available.

This package is runtime/client version `3.2.1` with `HLOG_ABI_VERSION = 3`.

3.2.1 fixes C++ bit-field logging in `HLOG_*`, `HLOG_CONSOLE_*`, direct Logger formatting and `QueryOutput::print`. Positional bit-fields can be passed directly; format checking and disabled-log laziness are retained. The fix is in `include/hlog.hpp`; existing ABI v3 runtimes remain compatible. See [release notes](docs/Release-3.2.1.md).
Hexdump formats add symbols without changing existing ABI layouts or signatures.
Install the new runtime before using formatted hexdump macros. New loaders probe for
console control symbols; old ABI v3 runtimes retain their existing commands but
reject `console` with an upgrade message. SONAME remains `libhlog.so.1`.

See [Console output](docs/Console.md) for HMP on/off, C/C++/Rust macros, colors,
and the complete HMP wrapper replacement, and [Buffer / Hexdump](docs/Hexdump.md)
for array/span macros, native-endian numeric groups and prefixed rows.
See [test instructions](tests/README.md) for executed checks and QEMU integration limits.

## Layout

```text
hlog/
├── runtime/                 Rust cdylib: libhlog.so.1
├── rust-client/             Rust frontend crate: hlog
├── include/
│   ├── hlog.h               C ABI and printf-style macros
│   └── hlog.hpp             C++ fmt wrapper and macros
├── qemu/
│   ├── hlog-loader.c
│   ├── hlog-loader.h
│   ├── meson.build.snippet
│   └── readconfig-integration.snippet
├── cmake/HLog.cmake
├── docs/HLog-Usage-Wiki.md
├── scripts/install.sh
├── scripts/verify_runtime.sh
└── examples/
```

面向模块开发、运维和 AI 自主诊断的完整使用规范见
[`docs/HLog-Usage-Wiki.md`](docs/HLog-Usage-Wiki.md)。

## Runtime behavior

The runtime constructor provides a usable fallback before any model is loaded:

- file: `./logs/qemu_device.log`
- default level: `Info`
- startup behavior: remove the current log and its `_1`, `_2`, ... rotation files; completed `.log.gz` archives remain
- rotation: 100 MiB per file, 10 files per batch (1000 MiB by default)
- archive: when the batch reaches its configured total size, detach it atomically, immediately continue in a new
  `qemu_device.log`, and gzip the detached files in a background thread
- flush: every 2 seconds; `Warn`, `Error`, and `Critical` flush immediately
- format: timestamp, level, logical logger, host thread id, function, and message

All registered loggers have independent levels and logical names, but share the same physical sink. Logger handles are
process-lifetime handles and intentionally have no unregister operation.

Completed batches use names such as `qemu_device.1787700000.12345.1.log.gz` (Unix time, process ID, sequence). Each gzip
stream contains the rotated files in oldest-to-newest order, so it remains plain line-oriented log text after
decompression. Search archives directly with `zgrep`:

```bash
zgrep -n -- "keyword" logs/qemu_device.*.log.gz
```

GNU `grep`/`ggrep` alone does not decompress gzip input. To search both the active log and every archive:

```bash
grep -n -- "keyword" logs/qemu_device.log
zgrep -n -- "keyword" logs/qemu_device.*.log.gz
```

Compression failure never blocks further logging. The detached uncompressed files are retained in a hidden
`.hlog-archive-*` staging directory and the error is reported on stderr. Completed archives are not automatically
deleted; archive-retention policy can therefore be managed separately according to the deployment's disk budget.

`HLogConfig.truncate` remains configurable. The fallback and every frontend default set it to `1`; explicitly set it to
`0` only when append behavior is required.

## Build and install

```bash
./scripts/install.sh
```

The installer is safe to invoke from another crate's `build.rs`. Before it
starts the nested Cargo process, it removes the outer Cargo/Make jobserver
variables. This prevents `early EOF on jobserver pipe`. A file lock also
serializes install attempts made concurrently by multiple model crates.

Nested runtime builds use one job by default. Set `HLOG_BUILD_JOBS` to a
positive integer when more parallelism is wanted:

```bash
HLOG_BUILD_JOBS=4 ./scripts/install.sh
```

The default install root is the source root itself, so the result is:

```text
hlog/lib/libhlog.so.1.0.0
hlog/lib/libhlog.so.1 -> libhlog.so.1.0.0
hlog/lib/libhlog.so -> libhlog.so.1
hlog/include/hlog.h
hlog/include/hlog.hpp
```

An optional separate prefix is also supported:

```bash
./scripts/install.sh /absolute/install/prefix
```

The runtime SONAME is `libhlog.so.1`. `scripts/verify_runtime.sh` is an install-time guard, not a runtime component.
`install.sh` invokes it after copying the library when the script is executable. It catches an incorrect SONAME or a
stale library that does not export the required ABI v3 symbols. Removing it would not change QEMU execution, but would
remove this post-install validation.

When integrated through `cmake/HLog.cmake`, a missing runtime is built and installed automatically with a fixed
`hlog/target` Cargo directory. Configure-time checks rebuild a missing, stale, or ABI-incompatible runtime. A generated
`runtime.stamp` and `hlog_runtime_build` target also place all Rust sources and build inputs in the Make/Ninja dependency
graph, so a normal incremental consumer build rebuilds hlog before relinking dependent models. Runtime state is kept
under the consumer's CMake build directory, not in the hlog source tree.

## QEMU preload

Copy `qemu/hlog-loader.h` to QEMU's `include/qemu/` directory and compile `qemu/hlog-loader.c` into the system emulator.
The integration deliberately has two phases because object plugins load before QEMU processes `-readconfig`:

```c
#include "qemu/hlog-loader.h"

/* Inside load_object_plugins(), before opening model DSOs. */
qemu_hlog_preload_from_env();

/* After the complete command-line/-readconfig option loop. */
if (!qemu_hlog_apply_config_rules(&error_fatal)) {
    exit(EXIT_FAILURE);
}
```

Launch QEMU through its existing config-file option:

```bash
HLOG_PATH=/absolute/path/to/hlog/lib/libhlog.so.1 \
qemu-system-aarch64 -readconfig /absolute/path/to/hlog.conf ...
```

With no `HLOG_PATH`, the early preload is a silent no-op and QEMU remains fully usable without hlog. Any hlog rules in
`-readconfig` are ignored when the optional runtime is unavailable. The loader opens the configured runtime globally
through GModule (without `G_MODULE_BIND_LOCAL`), resolves and validates the complete control ABI, calls
`hlog_init_default()`, and makes the module resident. QEMU neither includes `hlog.h` nor links against `libhlog`; every
model `DT_NEEDED` entry therefore resolves to the same process-global `libhlog.so.1` instance.

Direct loading is also available when the caller needs to decide how to handle an error:

```c
if (qemu_hlog_preload("/absolute/path/to/hlog/lib/libhlog.so.1") != 0) {
    /* Apply the product's desired optional-feature policy here. */
}
```

`qemu_hlog_preload_from_env()` reports an invalid configured path but deliberately does not terminate QEMU.

After a successful load, the loader dynamically registers QMP command `hlog`. The same exported handler is available
to the HMP `hlog` wrapper.

```json
{"execute":"hlog","arguments":{"action":"show"}}
{"execute":"hlog","arguments":{"action":"set","target":"all","level":"info"}}
{"execute":"hlog","arguments":{"action":"set","target":"bmu","level":"debug"}}
{"execute":"hlog","arguments":{"action":"set","target":"bmu/1","level":"trace"}}
{"execute":"hlog","arguments":{"action":"flush"}}
{"execute":"hlog","arguments":{"action":"query-list"}}
{"execute":"hlog","arguments":{"action":"query-help","target":"bmu/0/map-table"}}
{"execute":"hlog","arguments":{"action":"query","target":"bmu/0/map-table","args":"vf=3 l_pool=1"}}
```

`show` returns the active file and all registered logger names, modules, and levels. `set` accepts `all`, a module name,
or a full logger display name. See `qemu/meson.build.snippet` for the GModule build dependency and source-set hook, and
`qemu/hmp-query-integration.snippet` for the small argument-forwarding change to the existing HMP wrapper.

### ABI v3 startup levels with QEMU `-readconfig`

No custom QEMU command-line option is added. `hlog-loader.c` registers repeatable `[hlog "target"]` QemuOpts groups
through `opts_init()`. QEMU's existing `-readconfig` parser handles the file. After the main option loop, QEMU forwards
each rule to the ABI v3 runtime. Rules update loggers already created by object plugins and automatically apply to
future logger registrations.

```text
# hlog.conf
[hlog "all"]
level = "warn"

[hlog "bmu"]
level = "info"

[hlog "bmu/2"]
level = "debug"

[hlog "crg"]
level = "off"
```

Valid levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`, and `off`. Duplicate section IDs, malformed
QEMU config syntax, missing values, and invalid levels reject QEMU startup. File order does not affect the fixed
precedence: full logger name, then module name, then `all`, then the logger's creation fallback.

For example, `bmu = info` sets `bmu/1`, `bmu/2`, and every future `bmu/*` logger to `info`. An additional
`bmu/2 = debug` changes only that instance. Unknown modules and logger names are valid pending rules and begin working
automatically if those targets are registered later.

```bash
HLOG_PATH=/absolute/path/to/hlog/lib/libhlog.so.1 \
qemu-system-aarch64 \
    -M your-machine \
    -readconfig /absolute/path/to/hlog.conf \
    ...
```

`-readconfig` is QEMU's existing option and uses one leading dash. `HLOG_PATH` selects the process-global runtime early
enough for object-plugin loading. Without it, both the preload and later rule application are optional no-ops from
QEMU's availability perspective.

The corresponding QEMU 10.2 integration is:

```c
/* No qemu-options.hx change is needed. */
#include "qemu/hlog-loader.h"

/* Existing early hook inside load_object_plugins(), before model dlopen(). */
qemu_hlog_preload_from_env();

/* After the complete main option loop has processed -readconfig. */
if (!qemu_hlog_apply_config_rules(&error_fatal)) {
    exit(EXIT_FAILURE);
}
```

There is no `qemu-options.hx` entry, custom option-switch case, config parser, or machine-init-done notifier. Splitting
preload and rule application preserves the existing optional `load_object_plugins()` behavior without moving plugin
loading. Debug messages emitted before `-readconfig` is processed still use creation fallback levels; all later output
uses the applied rules. The remaining integration is the Meson entry in `qemu/meson.build.snippet`.

The `hlog` QMP command remains available. Its `set` operations update both current logger levels and the
in-memory rules used by future registrations. Setting a module replaces narrower logger overrides in that module;
setting `all` replaces all module and logger overrides. These runtime changes do not rewrite the source configuration
file. See `qemu/readconfig-integration.snippet` for the minimal QEMU hook and
`examples/hlog-readconfig.conf` for a ready-to-copy configuration.

## Module diagnostic queries

ABI v3 adds a process-global, read-only diagnostic query registry. A model registers a target in the form
`module/instance/name`, declares any number of named parameters, and supplies a callback. HMP/QMP pass one argument
string such as `vf=3 l_pool=1`; the runtime tokenizes it, validates it against the registered schema, and invokes the
callback with a dynamic argument array. Optional and repeatable parameters make the actual argument count variable.

The generic control command needs no BMU-specific QEMU code:

```text
(qemu) hlog query-list
(qemu) hlog query-list bmu
(qemu) hlog query-list bmu/0
(qemu) hlog query-help bmu/0/map-table
(qemu) hlog query bmu/0/map-table "vf=3 l_pool=1"
```

`query-list` without a filter lists every registered query. With `bmu`, it
lists only queries registered under the exact module name `bmu`; `bmu2` is
excluded. With `bmu/0`, it lists only that instance's queries. A full query
target matches only that query. Matching is case-sensitive, an optional
trailing `/` is supported for a module or instance, and no matches return an
empty list. Existing C/C++/Rust registrations do not need to change.

The module filter uses the existing QMP `target` argument:

```json
{"execute":"hlog","arguments":{"action":"query-list","target":"bmu"}}
```

For HMP, quote the final query argument string when it contains spaces. The corresponding QMP call is:

```json
{"execute":"hlog","arguments":{"action":"query","target":"bmu/0/map-table","args":"vf=3 l_pool=1"}}
```

The C registration surface is intentionally small:

```c
static const HLogQueryParamSpec params[] = {
    HLOG_QUERY_REQUIRED_U32("vf", "VF index, valid range 0..7"),
    HLOG_QUERY_OPTIONAL_U32("l_pool", "Optional logical pool filter"),
};

bmu->query = hlog_query_register(
    "bmu", "0", "map-table", "Show VF pool mappings",
    params, HLOG_ARRAY_SIZE(params), bmu_map_table_query, bmu);
```

C++ owns registration through a move-only RAII object and accepts a lambda:

```cpp
query_ = hlog::register_query(
    "bmu", "0", "map-table", "Show VF pool mappings",
    {hlog::required_u32("vf"), hlog::optional_u32("l_pool")},
    [this](const hlog::QueryArgs &args, hlog::QueryOutput &out) {
        auto vf = args.u32("vf");
        out.print("vf={} ...\n", vf);
    });
```

Rust uses the same shape without exposing FFI metadata:

```rust
let query = hlog::register_query("bmu", "0", "map-table")
    .description("Show VF pool mappings")
    .required_u32("vf", "VF index, valid range 0..7")
    .optional_u32("l_pool", "Optional logical pool filter")
    .register(move |args, output| {
        let vf = VfId::try_from(args.required_u32("vf")?)?;
        output.print(format_args!("vf={} ...\n", vf.raw()))?;
        Ok(())
    })?;
```

Store the returned C handle/C++ `QueryRegistration`/Rust `QueryRegistration` in the owning device instance and
unregister it before freeing device state. Unregistration removes the target first and waits for active callbacks to
finish, so the opaque device pointer cannot be used after destruction. A callback must not unregister itself. Query
output is limited to 1 MiB and raw arguments to 16 KiB. Unknown, duplicate, missing, and ill-typed parameters are
rejected before module code runs. See `examples/c_query_bmu.c`, `examples/cpp_query_bmu.cpp`, and
`examples/rust_model.rs` for complete examples.

## C API

C call sites use the same concise macro shape as C++ and keep printf formatting rules:

```c
#include "hlog.h"

HLogLogger *logger = hlog_create("subctrl", HLOG_LEVEL_INFO);
if (logger == NULL) {
    fprintf(stderr, "hlog create failed: %s\n", hlog_last_error());
    return;
}

uint32_t opcode = 0x17;
uint8_t packet[] = {0x01, 0x23, 0x45, 0x67};

HLOG_DEBUG(logger, "opcode=%#x", opcode);
HLOG_INFO(logger, "command submitted");
HLOG_HEXDUMP(logger, HLOG_LEVEL_DEBUG, packet, sizeof(packet));
```

Available macros are `HLOG_TRACE`, `HLOG_DEBUG`, `HLOG_INFO`, `HLOG_WARN`, `HLOG_ERROR`, `HLOG_CRITICAL`, and
`HLOG_HEXDUMP`. Disabled levels are checked before formatting, so their arguments are not evaluated.

`hlog_create_named(module, name, level)` creates a configured logical name. The lower-level
`hlog_register_logger(module, configured_name, level)` remains available for generic bindings.

## C++ API

C++ uses fmt formatting and the same macro names:

```cpp
#include "hlog.hpp"

class Bmu {
public:
    Bmu() : logger_("bmu", HLOG_LEVEL_INFO) {}

    void send(uint32_t opcode, const uint8_t *packet, size_t length)
    {
        HLOG_DEBUG(logger_, "opcode={:#x}", opcode);
        HLOG_INFO(logger_, "command submitted");
        HLOG_HEXDUMP(logger_, HLOG_LEVEL_DEBUG, packet, length);
    }

private:
    hlog::Logger logger_;
};
```

For standalone inclusion, `hlog.hpp` uses `<fmt/format.h>` by default. The supplied CMake targets automatically select
the project-local `<spdlog/fmt/fmt.h>` and propagate its include directory; consumers do not define anything manually.

## Hexdump

Every 16-byte row now gets a complete log prefix; rows from one dump remain contiguous.
The original `HLOG_HEXDUMP(logger, level, data, byte_length)` remains a byte view.
New C++17/C++20 macros infer the byte length of C arrays, `std::array`, `std::vector`, and `std::span`:

```cpp
HLOG_HEXDUMP_DEBUG(logger, hlog::u8, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u16, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u32, buffer);
HLOG_HEXDUMP_DEBUG(logger, hlog::u64, buffer);
HLOG_HEXDUMP_DEBUG(hlog::u32, buffer); // Process-default logger.
HLOG_HEXDUMP_DEBUG(logger, hlog::u32, pointer, byte_length);
```

Multi-byte groups use native byte order, support unaligned data, and show missing tail bytes as `??`.
ASCII columns align across all four formats. Empty input emits `<empty>`; there is no maximum buffer length.
The same macros exist at TRACE/INFO/WARN/ERROR/CRITICAL and with `HLOG_CONSOLE_HEXDUMP_*` for console-only output.
C uses `HLOG_HEXDUMP_DEBUG(logger, HLOG_DUMP_U32, pointer, byte_length)`.
Rust adds `lhexdump_debug!(logger, HexdumpFormat::U32, bytes)` and corresponding console/level variants.
All frontends use `hlog_hexdump_ex` / `hlog_console_hexdump_ex`; existing byte APIs remain available.
See [full usage and actual output](docs/Hexdump.md).

## Rust API

Rust device crates depend on the frontend crate and dynamically link the process-global runtime:

```toml
[dependencies]
hlog = { path = "/path/to/hlog/rust-client" }
```

The Rust client build script derives `lib/hlog/lib` from its own fixed repository location and automatically emits the
native link-search path. Device-model builds therefore require no link-path environment variable. Run
`lib/hlog/scripts/install.sh` once if `lib/hlog/lib/libhlog.so` has not been generated yet.

```rust
use hlog::{create_module_logger, ldebug, lhexdump, linfo, named, LogLevel, SharedLogger};

#[named]
fn send(logger: &SharedLogger, opcode: u32, packet: &[u8]) {
    ldebug!(logger, "opcode={:#x}", opcode);
    linfo!(logger, "command submitted");
    lhexdump!(logger, LogLevel::Debug, packet);
}

let logger = create_module_logger("bmu", LogLevel::Info)?;
```

The existing Rust macros remain `ltrace!`, `ldebug!`, `linfo!`, `lwarn!`, `lerror!`, `lcritical!`, and `lpanic!`;
`lhexdump!` is added. They retain `#[named]`/`function_name!()` support and avoid formatting disabled messages.

## CMake

```cmake
include("/project/lib/hlog/cmake/HLog.cmake")
target_link_libraries(bmu_model PRIVATE hlog::hlog)
```

The integration expects `lib/hlog` and `lib/spdlog` to be siblings. It resolves all paths automatically, validates both
public headers and `spdlog/fmt/fmt.h`, and verifies ABI v3 before reusing or accepting an installed runtime. Rust source
changes participate in incremental Make/Ninja builds through `hlog_runtime_build` and its build-directory stamp.

`hlog::runtime` is the imported shared-library target for C or generic consumers. `hlog::hlog` is the C++17 interface
target carrying the runtime dependency, hlog headers, sibling spdlog bundled-fmt headers, and the required compile
definition. Neither target links `libspdlog.so` or `libfmt.so`.

## Runtime control and lookup

The stable C ABI includes:

- `hlog_load_level_config` (persistent startup rules)
- `hlog_set_level_rule` (add or replace one persistent startup rule)
- `hlog_logger_set_level`, `hlog_set_default_level`, `hlog_set_module_level`, `hlog_set_all_levels`
- `hlog_module_count`, `hlog_module_name_at`, `hlog_module_logger_count`, `hlog_module_logger_at`
- `hlog_logger_module_name`, `hlog_logger_configured_name`, `hlog_logger_display_name`
- `hlog_flush`, `hlog_set_flush_interval_ms`, `hlog_file_path`, `hlog_shutdown`
- `hlog_query_register`, `hlog_query_unregister`, `hlog_query_execute`
- `hlog_query_count`, `hlog_query_target_at`, `hlog_query_description_at`, `hlog_query_usage_at`

No legacy naming compatibility aliases are included in this package.
