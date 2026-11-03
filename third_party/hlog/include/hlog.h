#ifndef HLOG_H
#define HLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HLOG_ABI_VERSION 3u

#define HLOG_OK                    0
#define HLOG_ERR_INVALID_ARGUMENT -1
#define HLOG_ERR_INTERNAL         -2
#define HLOG_ERR_NOT_FOUND        -3
#define HLOG_ERR_DUPLICATE        -4
#define HLOG_ERR_ABI_MISMATCH     -5
#define HLOG_ERR_BUSY             -6
#define HLOG_ERR_OUTPUT_TOO_LARGE -7

#define HLOG_QUERY_PARAM_REQUIRED   (1u << 0)
#define HLOG_QUERY_PARAM_REPEATABLE (1u << 1)
#define HLOG_QUERY_INVALID_HANDLE UINT64_C(0)

#define HLOG_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef enum HLogLevel {
    HLOG_LEVEL_TRACE = 0,
    HLOG_LEVEL_DEBUG = 1,
    HLOG_LEVEL_INFO = 2,
    HLOG_LEVEL_WARN = 3,
    HLOG_LEVEL_ERROR = 4,
    HLOG_LEVEL_CRITICAL = 5,
    HLOG_LEVEL_OFF = 6,
} HLogLevel;

/* Native-endian unsigned groups; values are group sizes in bytes. */
typedef enum HLogDumpFormat {
    HLOG_DUMP_U8 = 1,
    HLOG_DUMP_U16 = 2,
    HLOG_DUMP_U32 = 4,
    HLOG_DUMP_U64 = 8,
} HLogDumpFormat;

typedef struct HLogConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *base_dir;
    const char *file_name;
    uint32_t default_level;
    uint8_t truncate;
    uint8_t reserved[7];
    /* One file size and the number of files in each compressed archive batch. */
    uint64_t rotation_size_bytes;
    uint32_t max_files;
    uint32_t flush_interval_ms;
} HLogConfig;

typedef struct HLogLogger HLogLogger;

typedef uint64_t HLogQueryHandle;

typedef enum HLogQueryParamType {
    HLOG_QUERY_PARAM_STRING = 0,
    HLOG_QUERY_PARAM_U32 = 1,
    HLOG_QUERY_PARAM_U64 = 2,
    HLOG_QUERY_PARAM_I64 = 3,
    HLOG_QUERY_PARAM_BOOL = 4,
} HLogQueryParamType;

typedef struct HLogQueryParamSpec {
    const char *name;
    uint32_t type;
    uint32_t flags;
    const char *description;
} HLogQueryParamSpec;

typedef struct HLogQueryArg {
    const char *name;
    const char *value;
} HLogQueryArg;

typedef struct HLogQueryRequest {
    const HLogQueryArg *args;
    size_t arg_count;
} HLogQueryRequest;

typedef int32_t (*HLogQueryWriteFn)(void *opaque, const uint8_t *data, size_t length);
typedef int32_t (*HLogQueryCallback)(void *opaque, const HLogQueryRequest *request,
                                    HLogQueryWriteFn write, void *write_opaque);

#define HLOG_QUERY_REQUIRED_STRING(name, description) \
    { (name), HLOG_QUERY_PARAM_STRING, HLOG_QUERY_PARAM_REQUIRED, (description) }
#define HLOG_QUERY_OPTIONAL_STRING(name, description) \
    { (name), HLOG_QUERY_PARAM_STRING, 0, (description) }
#define HLOG_QUERY_REQUIRED_U32(name, description) \
    { (name), HLOG_QUERY_PARAM_U32, HLOG_QUERY_PARAM_REQUIRED, (description) }
#define HLOG_QUERY_OPTIONAL_U32(name, description) \
    { (name), HLOG_QUERY_PARAM_U32, 0, (description) }
#define HLOG_QUERY_REQUIRED_U64(name, description) \
    { (name), HLOG_QUERY_PARAM_U64, HLOG_QUERY_PARAM_REQUIRED, (description) }
#define HLOG_QUERY_OPTIONAL_U64(name, description) \
    { (name), HLOG_QUERY_PARAM_U64, 0, (description) }
#define HLOG_QUERY_REQUIRED_I64(name, description) \
    { (name), HLOG_QUERY_PARAM_I64, HLOG_QUERY_PARAM_REQUIRED, (description) }
#define HLOG_QUERY_OPTIONAL_I64(name, description) \
    { (name), HLOG_QUERY_PARAM_I64, 0, (description) }
#define HLOG_QUERY_REQUIRED_BOOL(name, description) \
    { (name), HLOG_QUERY_PARAM_BOOL, HLOG_QUERY_PARAM_REQUIRED, (description) }
#define HLOG_QUERY_OPTIONAL_BOOL(name, description) \
    { (name), HLOG_QUERY_PARAM_BOOL, 0, (description) }
#define HLOG_QUERY_REPEATABLE_U32(name, description) \
    { (name), HLOG_QUERY_PARAM_U32, HLOG_QUERY_PARAM_REPEATABLE, (description) }

uint32_t hlog_abi_version(void);
const char *hlog_last_error(void);
int32_t hlog_init_default(void);
int32_t hlog_init(const HLogConfig *config);

/*
 * Transactionally replace persistent level rules from a text file. Existing
 * loggers are updated immediately; future registrations inherit the same
 * exact-logger > module > all > creation-fallback precedence.
 */
int32_t hlog_load_level_config(const char *path);

/*
 * Add or replace one persistent rule without clearing narrower rules. This is
 * intended for structured configuration readers such as QEMU -readconfig.
 */
int32_t hlog_set_level_rule(const char *target, uint32_t level);

/*
 * Register one read-only diagnostic query at module/instance/name. The
 * runtime deep-copies all strings and parameter metadata before returning.
 * A zero handle indicates failure; hlog_last_error() contains the reason.
 */
HLogQueryHandle hlog_query_register(const char *module, const char *instance, const char *name,
                                    const char *description, const HLogQueryParamSpec *params,
                                    size_t param_count, HLogQueryCallback callback, void *opaque);
int32_t hlog_query_unregister(HLogQueryHandle handle);

size_t hlog_query_count(void);
size_t hlog_query_target_at(size_t index, char *buffer, size_t capacity);
size_t hlog_query_description_at(size_t index, char *buffer, size_t capacity);
size_t hlog_query_usage_at(size_t index, char *buffer, size_t capacity);
int32_t hlog_query_execute(const char *target, const char *args, HLogQueryWriteFn write,
                           void *write_opaque);

size_t hlog_query_arg_count(const HLogQueryRequest *request, const char *name);
const char *hlog_query_arg_at(const HLogQueryRequest *request, const char *name, size_t occurrence);
int32_t hlog_query_arg_u32(const HLogQueryRequest *request, const char *name, uint32_t *value);
int32_t hlog_query_arg_u64(const HLogQueryRequest *request, const char *name, uint64_t *value);
int32_t hlog_query_arg_i64(const HLogQueryRequest *request, const char *name, int64_t *value);
int32_t hlog_query_arg_bool(const HLogQueryRequest *request, const char *name, bool *value);
int32_t hlog_query_set_error(int32_t code, const char *message);

HLogLogger *hlog_register_logger(const char *module_name, const char *configured_name, uint32_t level);
HLogLogger *hlog_default_logger(void);
HLogLogger *hlog_get_logger(const char *module_name);

size_t hlog_module_logger_count(const char *module_name);
HLogLogger *hlog_module_logger_at(const char *module_name, size_t index);
size_t hlog_module_count(void);
size_t hlog_module_name_at(size_t index, char *buffer, size_t capacity);
size_t hlog_logger_module_name(const HLogLogger *logger, char *buffer, size_t capacity);
size_t hlog_logger_configured_name(const HLogLogger *logger, char *buffer, size_t capacity);
size_t hlog_logger_display_name(const HLogLogger *logger, char *buffer, size_t capacity);
size_t hlog_logger_instance_id(const HLogLogger *logger);
uint32_t hlog_logger_level(const HLogLogger *logger);
int32_t hlog_logger_config_name(HLogLogger *logger, const char *new_name);

bool hlog_enabled(const HLogLogger *logger, uint32_t level);
/* Since runtime 3.1.0 (additive ABI v3 extension).
 * Mirroring defaults off, applies to all current/future file loggers, and
 * respects their levels. Console-only writes bypass file levels and mirroring,
 * do not write files, and use stderr (not the guest UART). OFF emits nothing.
 * ANSI level colors are automatic on a TTY; redirected output is plain text.
 */
int32_t hlog_set_console_enabled(bool enabled);
bool hlog_console_enabled(void);
int32_t hlog_console_write(const HLogLogger *logger, uint32_t level,
                          const uint8_t *function, size_t function_len,
                          const uint8_t *file, size_t file_len, uint32_t line,
                          const uint8_t *message, size_t message_len);
int32_t hlog_console_hexdump(const HLogLogger *logger, uint32_t level,
                            const uint8_t *function, size_t function_len,
                            const uint8_t *file, size_t file_len, uint32_t line,
                            const uint8_t *data, size_t data_len);
int32_t hlog_write(const HLogLogger *logger, uint32_t level, const uint8_t *function, size_t function_len,
                   const uint8_t *file, size_t file_len, uint32_t line, const uint8_t *message,
                   size_t message_len);
int32_t hlog_hexdump(const HLogLogger *logger, uint32_t level, const uint8_t *function, size_t function_len,
                     const uint8_t *file, size_t file_len, uint32_t line, const uint8_t *data, size_t data_len);

/* Since 3.2.0, additive ABI v3 symbols. Existing hexdump APIs use U8.
 * Each 16-byte row is a separate prefixed record; dumps are serialized as a
 * batch. data_len is always bytes, independent of format (1/2/4/8).
 * Partial final groups use ?? for missing bytes; unaligned data is supported.
 */
int32_t hlog_hexdump_ex(const HLogLogger *logger, uint32_t level,
                       const uint8_t *function, size_t function_len,
                       const uint8_t *file, size_t file_len, uint32_t line,
                       const uint8_t *data, size_t data_len, uint32_t format);
int32_t hlog_console_hexdump_ex(const HLogLogger *logger, uint32_t level,
                               const uint8_t *function, size_t function_len,
                               const uint8_t *file, size_t file_len, uint32_t line,
                               const uint8_t *data, size_t data_len, uint32_t format);

int32_t hlog_logger_set_level(HLogLogger *logger, uint32_t level);
int32_t hlog_set_default_level(uint32_t level);
int32_t hlog_set_module_level(const char *module_name, uint32_t level);
int32_t hlog_set_all_levels(uint32_t level);
int32_t hlog_flush(void);
int32_t hlog_set_flush_interval_ms(uint32_t interval_ms);
size_t hlog_file_path(char *buffer, size_t capacity);
void hlog_shutdown(void);

#ifdef __cplusplus
}
#else

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline HLogLogger *hlog_create(const char *module_name, HLogLevel level)
{
    return hlog_register_logger(module_name, NULL, (uint32_t)level);
}

static inline HLogLogger *hlog_create_named(const char *module_name, const char *name, HLogLevel level)
{
    return hlog_register_logger(module_name, name, (uint32_t)level);
}

static inline int32_t hlog_query_printf(HLogQueryWriteFn write, void *opaque, const char *format, ...)
{
    char local[512];
    va_list args;
    va_list retry_args;
    int required;

    if (write == NULL || format == NULL) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }

    va_start(args, format);
    va_copy(retry_args, args);
    required = vsnprintf(local, sizeof(local), format, args);
    va_end(args);
    if (required < 0) {
        va_end(retry_args);
        return HLOG_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)required < sizeof(local)) {
        va_end(retry_args);
        return write(opaque, (const uint8_t *)local, (size_t)required);
    }

    size_t capacity = (size_t)required + 1;
    char *message = (char *)malloc(capacity);
    if (message == NULL) {
        va_end(retry_args);
        return HLOG_ERR_INTERNAL;
    }
    (void)vsnprintf(message, capacity, format, retry_args);
    va_end(retry_args);
    int32_t rc = write(opaque, (const uint8_t *)message, (size_t)required);
    free(message);
    return rc;
}

static inline int32_t hlog_printf(const HLogLogger *logger, HLogLevel level, const char *function, const char *file,
                                  uint32_t line, const char *format, ...)
{
    char local[512];
    va_list args;
    va_list retry_args;

    if (format == NULL) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }

    va_start(args, format);
    va_copy(retry_args, args);
    int required = vsnprintf(local, sizeof(local), format, args);
    va_end(args);

    if (required < 0) {
        va_end(retry_args);
        return HLOG_ERR_INVALID_ARGUMENT;
    }

    if ((size_t)required < sizeof(local)) {
        va_end(retry_args);
        return hlog_write(logger, (uint32_t)level, (const uint8_t *)function, function ? strlen(function) : 0,
                          (const uint8_t *)file, file ? strlen(file) : 0, line, (const uint8_t *)local,
                          (size_t)required);
    }

    size_t capacity = (size_t)required + 1;
    char *message = (char *)malloc(capacity);
    if (message == NULL) {
        va_end(retry_args);
        return HLOG_ERR_INTERNAL;
    }

    (void)vsnprintf(message, capacity, format, retry_args);
    va_end(retry_args);
    int32_t rc = hlog_write(logger, (uint32_t)level, (const uint8_t *)function,
                            function ? strlen(function) : 0, (const uint8_t *)file, file ? strlen(file) : 0, line,
                            (const uint8_t *)message, (size_t)required);
    free(message);
    return rc;
}

/* Format first, then route once to console. No hlog_enabled() check here. */
static inline int32_t hlog_console_printf(const HLogLogger *logger, HLogLevel level,
                                         const char *function, const char *file,
                                         uint32_t line, const char *format, ...)
{
    char local[512];
    char *message = local;
    va_list args, retry_args;
    int required;
    int32_t rc;

    if (format == NULL) {
        return HLOG_ERR_INVALID_ARGUMENT;
    }
    va_start(args, format);
    va_copy(retry_args, args);
    required = vsnprintf(local, sizeof(local), format, args);
    va_end(args);
    if (required < 0) {
        va_end(retry_args);
        return HLOG_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)required >= sizeof(local)) {
        message = (char *)malloc((size_t)required + 1);
        if (message == NULL) {
            va_end(retry_args);
            return HLOG_ERR_INTERNAL;
        }
        (void)vsnprintf(message, (size_t)required + 1, format, retry_args);
    }
    va_end(retry_args);
    rc = hlog_console_write(logger, (uint32_t)level,
                            (const uint8_t *)function, function ? strlen(function) : 0,
                            (const uint8_t *)file, file ? strlen(file) : 0, line,
                            (const uint8_t *)message, (size_t)required);
    if (message != local) {
        free(message);
    }
    return rc;
}

#define HLOG_CONSOLE_IMPL(logger_expr, level_value, format_string, ...)           \
    do {                                                                       \
        const HLogLogger *hlog_logger_value = (logger_expr);                     \
        HLogLevel hlog_level_value = (level_value);                              \
        if (hlog_logger_value && hlog_level_value != HLOG_LEVEL_OFF) {            \
            (void)hlog_console_printf(hlog_logger_value, hlog_level_value,       \
                __func__, __FILE__, (uint32_t)__LINE__,                          \
                (format_string), ##__VA_ARGS__);                                \
        }                                                                      \
    } while (0)

#define HLOG_CONSOLE_HEXDUMP(logger_expr, level_value, data_value, data_length)   \
    do {                                                                       \
        const HLogLogger *hlog_logger_value = (logger_expr);                     \
        HLogLevel hlog_level_value = (level_value);                              \
        if (hlog_logger_value && hlog_level_value != HLOG_LEVEL_OFF) {            \
            (void)hlog_console_hexdump(hlog_logger_value, hlog_level_value,      \
                (const uint8_t *)__func__, strlen(__func__),                     \
                (const uint8_t *)__FILE__, strlen(__FILE__),                     \
                (uint32_t)__LINE__, (const uint8_t *)(data_value),               \
                (size_t)(data_length));                                         \
        }                                                                      \
    } while (0)

#define HLOG_IMPL(logger_expr, level_value, format_string, ...)                                         \
    do {                                                                                               \
        const HLogLogger *hlog_logger_value = (logger_expr);                                            \
        HLogLevel hlog_level_value = (level_value);                                                     \
        if (hlog_enabled(hlog_logger_value, (uint32_t)hlog_level_value)) {                              \
            (void)hlog_printf(hlog_logger_value, hlog_level_value, __func__, __FILE__, (uint32_t)__LINE__, \
                              (format_string), ##__VA_ARGS__);                                          \
        }                                                                                              \
    } while (0)

#define HLOG_CONSOLE_TRACE(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_TRACE, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_DEBUG(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_DEBUG, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_INFO(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_INFO, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_WARN(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_WARN, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_ERROR(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_ERROR, format_string, ##__VA_ARGS__)
#define HLOG_CONSOLE_CRITICAL(logger, format_string, ...) \
    HLOG_CONSOLE_IMPL((logger), HLOG_LEVEL_CRITICAL, format_string, ##__VA_ARGS__)

#define HLOG_TRACE(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_TRACE, format_string, ##__VA_ARGS__)
#define HLOG_DEBUG(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_DEBUG, format_string, ##__VA_ARGS__)
#define HLOG_INFO(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_INFO, format_string, ##__VA_ARGS__)
#define HLOG_WARN(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_WARN, format_string, ##__VA_ARGS__)
#define HLOG_ERROR(logger, format_string, ...) HLOG_IMPL((logger), HLOG_LEVEL_ERROR, format_string, ##__VA_ARGS__)
#define HLOG_CRITICAL(logger, format_string, ...)                                                           \
    HLOG_IMPL((logger), HLOG_LEVEL_CRITICAL, format_string, ##__VA_ARGS__)

#define HLOG_HEXDUMP(logger_expr, level_value, data_value, data_length)                                  \
    do {                                                                                                \
        const HLogLogger *hlog_logger_value = (logger_expr);                                             \
        HLogLevel hlog_level_value = (level_value);                                                      \
        if (hlog_enabled(hlog_logger_value, (uint32_t)hlog_level_value)) {                               \
            (void)hlog_hexdump(hlog_logger_value, (uint32_t)hlog_level_value, (const uint8_t *)__func__, \
                               strlen(__func__), (const uint8_t *)__FILE__, strlen(__FILE__),             \
                               (uint32_t)__LINE__, (const uint8_t *)(data_value), (size_t)(data_length)); \
        }                                                                                               \
    } while (0)

/* C callers pass an explicit byte count. C++ adds array/span overloads. */
#define HLOG_DETAIL_DUMP_FORMAT_IMPL(logger_expr, level_value, format_value, data_value, data_length, console_only) \
    do { \
        const HLogLogger *hlog_dump_logger = (logger_expr); \
        HLogLevel hlog_dump_level = (level_value); \
        if ((console_only) ? (hlog_dump_logger && hlog_dump_level != HLOG_LEVEL_OFF) \
                           : hlog_enabled(hlog_dump_logger, (uint32_t)hlog_dump_level)) { \
            (void)((console_only) ? hlog_console_hexdump_ex : hlog_hexdump_ex)( \
                hlog_dump_logger, (uint32_t)hlog_dump_level, \
                (const uint8_t *)__func__, strlen(__func__), \
                (const uint8_t *)__FILE__, strlen(__FILE__), (uint32_t)__LINE__, \
                (const uint8_t *)(data_value), (size_t)(data_length), (uint32_t)(format_value)); \
        } \
    } while (0)

#define HLOG_HEXDUMP_TRACE(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_TRACE, format, data, length, 0)
#define HLOG_HEXDUMP_DEBUG(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_DEBUG, format, data, length, 0)
#define HLOG_HEXDUMP_INFO(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_INFO, format, data, length, 0)
#define HLOG_HEXDUMP_WARN(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_WARN, format, data, length, 0)
#define HLOG_HEXDUMP_ERROR(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_ERROR, format, data, length, 0)
#define HLOG_HEXDUMP_CRITICAL(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_CRITICAL, format, data, length, 0)
#define HLOG_CONSOLE_HEXDUMP_TRACE(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_TRACE, format, data, length, 1)
#define HLOG_CONSOLE_HEXDUMP_DEBUG(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_DEBUG, format, data, length, 1)
#define HLOG_CONSOLE_HEXDUMP_INFO(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_INFO, format, data, length, 1)
#define HLOG_CONSOLE_HEXDUMP_WARN(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_WARN, format, data, length, 1)
#define HLOG_CONSOLE_HEXDUMP_ERROR(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_ERROR, format, data, length, 1)
#define HLOG_CONSOLE_HEXDUMP_CRITICAL(logger, format, data, length) HLOG_DETAIL_DUMP_FORMAT_IMPL(logger, HLOG_LEVEL_CRITICAL, format, data, length, 1)

#endif

#endif
