/*
 * Optional hlog preload helper.
 *
 * Copyright (c) 2026-, Huawei
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * QEMU does not build-depend on the external hlog repository. This loader
 * registers QemuOpts groups for the existing -readconfig parser and uses
 * GModule to resolve the optional runtime ABI. It never includes hlog.h or
 * directly links an hlog_* symbol.
 *
 *   QEMU -- g_module_open(path, global) --> libhlog.so.1
 *                                            ^   ^   ^
 *                                            |   |   |
 *                                      model_a model_b model_c
 *
 * The runtime is made resident, so every model resolves its DT_NEEDED entry to
 * the same process-global libhlog.so.1 instance.
 */

#include "qemu/osdep.h"
#include <gmodule.h>

#include "monitor/monitor-internal.h"
#include "qapi/error.h"
#include "qapi/qmp-registry.h"
#include "qemu/config-file.h"
#include "qemu/hlog-loader.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qobject.h"

/* QEMU-local minimal view of the hlog ABI. Do not include external hlog.h. */
#define HLOG_EXPECTED_ABI_VERSION 3u
#define HLOG_OK 0

typedef enum HLogLevel {
    HLOG_LEVEL_TRACE = 0,
    HLOG_LEVEL_DEBUG = 1,
    HLOG_LEVEL_INFO = 2,
    HLOG_LEVEL_WARN = 3,
    HLOG_LEVEL_ERROR = 4,
    HLOG_LEVEL_CRITICAL = 5,
    HLOG_LEVEL_OFF = 6,
} HLogLevel;

typedef struct HLogLogger HLogLogger;

typedef uint32_t (*HLogAbiVersionFn)(void);
typedef const char *(*HLogLastErrorFn)(void);
typedef int32_t (*HLogInitDefaultFn)(void);
typedef int32_t (*HLogSetLevelRuleFn)(const char *target, uint32_t level);
typedef size_t (*HLogModuleCountFn)(void);
typedef size_t (*HLogModuleNameAtFn)(size_t index, char *buffer, size_t capacity);
typedef size_t (*HLogModuleLoggerCountFn)(const char *module_name);
typedef HLogLogger *(*HLogModuleLoggerAtFn)(const char *module_name, size_t index);
typedef size_t (*HLogLoggerModuleNameFn)(const HLogLogger *logger, char *buffer, size_t capacity);
typedef size_t (*HLogLoggerDisplayNameFn)(const HLogLogger *logger, char *buffer, size_t capacity);
typedef uint32_t (*HLogLoggerLevelFn)(const HLogLogger *logger);
typedef int32_t (*HLogLoggerSetLevelFn)(HLogLogger *logger, uint32_t level);
typedef int32_t (*HLogSetModuleLevelFn)(const char *module_name, uint32_t level);
typedef int32_t (*HLogSetAllLevelsFn)(uint32_t level);
typedef int32_t (*HLogFlushFn)(void);
typedef int32_t (*HLogSetConsoleEnabledFn)(bool enabled);
typedef bool (*HLogConsoleEnabledFn)(void);
typedef size_t (*HLogFilePathFn)(char *buffer, size_t capacity);
typedef int32_t (*HLogQueryWriteFn)(void *opaque, const uint8_t *data, size_t length);
typedef size_t (*HLogQueryCountFn)(void);
typedef size_t (*HLogQueryStringAtFn)(size_t index, char *buffer, size_t capacity);
typedef int32_t (*HLogQueryExecuteFn)(const char *target, const char *args, HLogQueryWriteFn write,
                                     void *write_opaque);

typedef struct HLogOps {
    HLogAbiVersionFn abi_version;
    HLogLastErrorFn last_error;
    HLogInitDefaultFn init_default;
    HLogSetLevelRuleFn set_level_rule;
    HLogModuleCountFn module_count;
    HLogModuleNameAtFn module_name_at;
    HLogModuleLoggerCountFn module_logger_count;
    HLogModuleLoggerAtFn module_logger_at;
    HLogLoggerModuleNameFn logger_module_name;
    HLogLoggerDisplayNameFn logger_display_name;
    HLogLoggerLevelFn logger_level;
    HLogLoggerSetLevelFn logger_set_level;
    HLogSetModuleLevelFn set_module_level;
    HLogSetAllLevelsFn set_all_levels;
    HLogFlushFn flush;
    HLogSetConsoleEnabledFn set_console_enabled;
    HLogConsoleEnabledFn console_enabled;
    HLogFilePathFn file_path;
    HLogQueryCountFn query_count;
    HLogQueryStringAtFn query_target_at;
    HLogQueryStringAtFn query_description_at;
    HLogQueryStringAtFn query_usage_at;
    HLogQueryExecuteFn query_execute;
} HLogOps;

static GModule *hlog_module;
static HLogOps hlog_ops;
static bool hlog_ready;
static bool hlog_qmp_registered;

static QemuOptsList hlog_rule_opts = {
    .name = "hlog",
    .head = QTAILQ_HEAD_INITIALIZER(hlog_rule_opts.head),
    .desc = {
        {
            .name = "level",
            .type = QEMU_OPT_STRING,
        },
        { /* end of list */ }
    },
};

static void hlog_register_config_opts(void)
{
    qemu_add_opts(&hlog_rule_opts);
}

opts_init(hlog_register_config_opts);

static void hlog_early_report(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void hlog_early_report(const char *fmt, ...)
{
    va_list ap;

    fputs("hlog: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *hlog_last_error(void)
{
    const char *error;

    if (!hlog_ops.last_error) {
        return "unknown hlog error";
    }
    error = hlog_ops.last_error();
    return error && *error ? error : "unknown hlog error";
}

static const char *hlog_level_name(uint32_t level)
{
    switch (level) {
    case HLOG_LEVEL_TRACE:
        return "trace";
    case HLOG_LEVEL_DEBUG:
        return "debug";
    case HLOG_LEVEL_INFO:
        return "info";
    case HLOG_LEVEL_WARN:
        return "warn";
    case HLOG_LEVEL_ERROR:
        return "error";
    case HLOG_LEVEL_CRITICAL:
        return "critical";
    case HLOG_LEVEL_OFF:
        return "off";
    default:
        return "unknown";
    }
}

static bool hlog_parse_level(const char *text, uint32_t *level)
{
    if (!g_ascii_strcasecmp(text, "trace")) {
        *level = HLOG_LEVEL_TRACE;
    } else if (!g_ascii_strcasecmp(text, "debug")) {
        *level = HLOG_LEVEL_DEBUG;
    } else if (!g_ascii_strcasecmp(text, "info")) {
        *level = HLOG_LEVEL_INFO;
    } else if (!g_ascii_strcasecmp(text, "warn") || !g_ascii_strcasecmp(text, "warning")) {
        *level = HLOG_LEVEL_WARN;
    } else if (!g_ascii_strcasecmp(text, "error")) {
        *level = HLOG_LEVEL_ERROR;
    } else if (!g_ascii_strcasecmp(text, "critical")) {
        *level = HLOG_LEVEL_CRITICAL;
    } else if (!g_ascii_strcasecmp(text, "off")) {
        *level = HLOG_LEVEL_OFF;
    } else {
        return false;
    }
    return true;
}

/* String-copy ABI: query required capacity including NUL, allocate, then copy. */
static char *hlog_module_name_dup(size_t index)
{
    size_t required = hlog_ops.module_name_at(index, NULL, 0);
    char *buffer;

    if (required == 0) {
        return NULL;
    }
    buffer = g_malloc(required);
    if (hlog_ops.module_name_at(index, buffer, required) == 0) {
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

static char *hlog_logger_module_name_dup(const HLogLogger *logger)
{
    size_t required = hlog_ops.logger_module_name(logger, NULL, 0);
    char *buffer;

    if (required == 0) {
        return NULL;
    }
    buffer = g_malloc(required);
    if (hlog_ops.logger_module_name(logger, buffer, required) == 0) {
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

static char *hlog_logger_display_name_dup(const HLogLogger *logger)
{
    size_t required = hlog_ops.logger_display_name(logger, NULL, 0);
    char *buffer;

    if (required == 0) {
        return NULL;
    }
    buffer = g_malloc(required);
    if (hlog_ops.logger_display_name(logger, buffer, required) == 0) {
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

static char *hlog_file_path_dup(void)
{
    size_t required = hlog_ops.file_path(NULL, 0);
    char *buffer;

    if (required == 0) {
        return NULL;
    }
    buffer = g_malloc(required);
    if (hlog_ops.file_path(buffer, required) == 0) {
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

static char *hlog_query_string_dup(HLogQueryStringAtFn get, size_t index)
{
    size_t required = get(index, NULL, 0);
    char *buffer;

    if (required == 0) {
        return NULL;
    }
    buffer = g_malloc(required);
    if (get(index, buffer, required) == 0) {
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

/* Control-plane linear lookup by full display name, for example bmu/1. */
static HLogLogger *hlog_find_logger(const char *display_name)
{
    size_t module_count;
    size_t i;

    if (!display_name || !*display_name) {
        return NULL;
    }

    module_count = hlog_ops.module_count();
    for (i = 0; i < module_count; i++) {
        g_autofree char *module_name = hlog_module_name_dup(i);
        size_t logger_count;
        size_t j;

        if (!module_name) {
            continue;
        }

        logger_count = hlog_ops.module_logger_count(module_name);
        for (j = 0; j < logger_count; j++) {
            HLogLogger *logger = hlog_ops.module_logger_at(module_name, j);
            g_autofree char *name = NULL;

            if (!logger) {
                continue;
            }
            name = hlog_logger_display_name_dup(logger);
            if (name && !strcmp(name, display_name)) {
                return logger;
            }
        }
    }
    return NULL;
}

static bool hlog_qmp_validate_args(const QDict *args, Error **errp)
{
    const QDictEntry *entry;

    for (entry = qdict_first(args); entry; entry = qdict_next(args, entry)) {
        const char *key = qdict_entry_key(entry);

        if (!strcmp(key, "action") || !strcmp(key, "target") || !strcmp(key, "level") ||
            !strcmp(key, "args")) {
            continue;
        }
        error_setg(errp, "hlog: unexpected argument '%s'", key);
        return false;
    }
    return true;
}

static void hlog_qmp_show(QObject **ret, Error **errp)
{
    g_autoptr(QDict) result = qdict_new();
    g_autoptr(QList) logger_list = qlist_new();
    g_autofree char *log_file = hlog_file_path_dup();
    size_t module_count;
    size_t i;

    (void)errp;
    qdict_put_bool(result, "loaded", true);
    qdict_put_bool(result, "console_supported", hlog_ops.console_enabled != NULL);
    qdict_put_bool(result, "console_enabled",
                   hlog_ops.console_enabled && hlog_ops.console_enabled());
    qdict_put_str(result, "file", log_file ? log_file : "");

    module_count = hlog_ops.module_count();
    for (i = 0; i < module_count; i++) {
        g_autofree char *module_name = hlog_module_name_dup(i);
        size_t logger_count;
        size_t j;

        if (!module_name) {
            continue;
        }

        logger_count = hlog_ops.module_logger_count(module_name);
        for (j = 0; j < logger_count; j++) {
            HLogLogger *logger = hlog_ops.module_logger_at(module_name, j);
            g_autofree char *display_name = NULL;
            g_autofree char *real_module_name = NULL;
            QDict *logger_info;
            uint32_t level;

            if (!logger) {
                continue;
            }

            display_name = hlog_logger_display_name_dup(logger);
            real_module_name = hlog_logger_module_name_dup(logger);
            if (!display_name) {
                continue;
            }

            level = hlog_ops.logger_level(logger);
            logger_info = qdict_new();
            qdict_put_str(logger_info, "name", display_name);
            qdict_put_str(logger_info, "module", real_module_name ? real_module_name : module_name);
            qdict_put_str(logger_info, "level", hlog_level_name(level));
            qlist_append(logger_list, logger_info);
        }
    }

    qdict_put(result, "loggers", g_steal_pointer(&logger_list));
    *ret = QOBJECT(g_steal_pointer(&result));
}

static void hlog_qmp_set(const QDict *args, QObject **ret, Error **errp)
{
    const char *target = qdict_get_try_str(args, "target");
    const char *level_text = qdict_get_try_str(args, "level");
    g_autoptr(QDict) result = NULL;
    uint32_t level;
    int32_t rc;

    if (!target || !*target) {
        error_setg(errp, "hlog set requires 'target'");
        return;
    }
    if (!level_text || !*level_text) {
        error_setg(errp, "hlog set requires 'level'");
        return;
    }
    if (!hlog_parse_level(level_text, &level)) {
        error_setg(errp, "invalid log level '%s'; expected trace/debug/info/warn/error/critical/off", level_text);
        return;
    }

    if (!g_ascii_strcasecmp(target, "all")) {
        rc = hlog_ops.set_all_levels(level);
        if (rc != HLOG_OK) {
            error_setg(errp, "failed to set all log levels: %s", hlog_last_error());
            return;
        }
    } else if (strchr(target, '/')) {
        HLogLogger *logger = hlog_find_logger(target);

        if (!logger) {
            error_setg(errp, "logger '%s' not found", target);
            return;
        }
        rc = hlog_ops.logger_set_level(logger, level);
        if (rc != HLOG_OK) {
            error_setg(errp, "failed to set logger '%s': %s", target, hlog_last_error());
            return;
        }
    } else {
        rc = hlog_ops.set_module_level(target, level);
        if (rc != HLOG_OK) {
            error_setg(errp, "failed to set module '%s': %s", target, hlog_last_error());
            return;
        }
    }

    result = qdict_new();
    qdict_put_str(result, "action", "set");
    qdict_put_str(result, "target", target);
    qdict_put_str(result, "level", hlog_level_name(level));
    *ret = QOBJECT(g_steal_pointer(&result));
}

static void hlog_qmp_console(const QDict *args, QObject **ret, Error **errp)
{
    const char *target = qdict_get_try_str(args, "target");
    g_autoptr(QDict) result = NULL;
    bool enabled;

    if (!hlog_ops.set_console_enabled || !hlog_ops.console_enabled) {
        error_setg(errp, "hlog console requires runtime 3.1.0 or newer");
        return;
    }
    if (qdict_haskey(args, "level") || qdict_haskey(args, "args") ||
        (qdict_haskey(args, "target") && !target)) {
        error_setg(errp, "hlog console expects only an optional on/off/show target");
        return;
    }
    if (target && g_ascii_strcasecmp(target, "show")) {
        if (!g_ascii_strcasecmp(target, "on")) {
            enabled = true;
        } else if (!g_ascii_strcasecmp(target, "off")) {
            enabled = false;
        } else {
            error_setg(errp, "hlog console expects on/off/show, got '%s'", target);
            return;
        }
        if (hlog_ops.set_console_enabled(enabled) != HLOG_OK) {
            error_setg(errp, "hlog console failed: %s", hlog_last_error());
            return;
        }
    }
    result = qdict_new();
    qdict_put_str(result, "action", "console");
    qdict_put_bool(result, "console_enabled", hlog_ops.console_enabled());
    qdict_put_str(result, "destination", "stderr");
    *ret = QOBJECT(g_steal_pointer(&result));
}

static void hlog_qmp_flush(QObject **ret, Error **errp)
{
    g_autoptr(QDict) result = NULL;
    int32_t rc = hlog_ops.flush();

    if (rc != HLOG_OK) {
        error_setg(errp, "hlog flush failed: %s", hlog_last_error());
        return;
    }

    result = qdict_new();
    qdict_put_str(result, "action", "flush");
    qdict_put_bool(result, "flushed", true);
    *ret = QOBJECT(g_steal_pointer(&result));
}

static bool hlog_query_find_index(const char *target, size_t *index)
{
    size_t count = hlog_ops.query_count();
    size_t i;

    for (i = 0; i < count; i++) {
        g_autofree char *candidate = hlog_query_string_dup(hlog_ops.query_target_at, i);

        if (candidate && !strcmp(candidate, target)) {
            *index = i;
            return true;
        }
    }
    return false;
}

static QDict *hlog_query_info(size_t index)
{
    g_autofree char *target = hlog_query_string_dup(hlog_ops.query_target_at, index);
    g_autofree char *description = hlog_query_string_dup(hlog_ops.query_description_at, index);
    g_autofree char *usage = hlog_query_string_dup(hlog_ops.query_usage_at, index);
    QDict *info;

    if (!target) {
        return NULL;
    }
    info = qdict_new();
    qdict_put_str(info, "target", target);
    qdict_put_str(info, "description", description ? description : "");
    qdict_put_str(info, "usage", usage ? usage : target);
    return info;
}

/* Match complete path components, not arbitrary string prefixes. */
static bool hlog_query_matches_filter(const char *target, const char *filter)
{
    size_t length;

    if (!filter || !*filter) {
        return true;
    }
    if (!target) {
        return false;
    }
    length = strlen(filter);
    if (strncmp(target, filter, length)) {
        return false;
    }
    return filter[length - 1] == '/' || target[length] == '/' ||
           target[length] == '\0';
}

static void hlog_qmp_query_list(const QDict *args, QObject **ret, Error **errp)
{
    const char *filter = qdict_get_try_str(args, "target");
    g_autoptr(QDict) result = qdict_new();
    g_autoptr(QList) queries = qlist_new();
    size_t count = hlog_ops.query_count();
    size_t i;

    (void)errp;
    for (i = 0; i < count; i++) {
        QDict *info = hlog_query_info(i);
        const char *target;

        if (!info) {
            continue;
        }
        target = qdict_get_try_str(info, "target");
        if (!hlog_query_matches_filter(target, filter)) {
            qobject_unref(QOBJECT(info));
            continue;
        }
        qlist_append(queries, info);
    }

    qdict_put_str(result, "action", "query-list");
    qdict_put(result, "queries", g_steal_pointer(&queries));
    *ret = QOBJECT(g_steal_pointer(&result));
}

static void hlog_qmp_query_help(const QDict *args, QObject **ret, Error **errp)
{
    const char *target = qdict_get_try_str(args, "target");
    g_autoptr(QDict) result = qdict_new();
    size_t index;
    QDict *info;

    if (!target || !*target) {
        error_setg(errp, "hlog query-help requires 'target'");
        return;
    }
    if (!hlog_query_find_index(target, &index)) {
        error_setg(errp, "hlog query '%s' not found", target);
        return;
    }
    info = hlog_query_info(index);
    if (!info) {
        error_setg(errp, "failed to inspect hlog query '%s': %s", target, hlog_last_error());
        return;
    }
    qdict_put_str(result, "action", "query-help");
    qdict_put(result, "query", info);
    *ret = QOBJECT(g_steal_pointer(&result));
}

typedef struct HLogQueryOutputBuffer {
    char *data;
    size_t length;
    size_t capacity;
} HLogQueryOutputBuffer;

static int32_t hlog_qemu_query_write(void *opaque, const uint8_t *data, size_t length)
{
    HLogQueryOutputBuffer *output = opaque;
    size_t required;
    size_t capacity;

    if (!output || (length != 0 && !data)) {
        return -1;
    }
    if (length > SIZE_MAX - output->length - 1) {
        return -1;
    }
    required = output->length + length + 1;
    if (required > output->capacity) {
        capacity = output->capacity ? output->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        output->data = g_realloc_n(output->data, capacity, 1);
        output->capacity = capacity;
    }
    memcpy(output->data + output->length, data, length);
    output->length += length;
    output->data[output->length] = '\0';
    return HLOG_OK;
}

static void hlog_qmp_query(const QDict *args, QObject **ret, Error **errp)
{
    const char *target = qdict_get_try_str(args, "target");
    const char *query_args = qdict_get_try_str(args, "args");
    HLogQueryOutputBuffer output = { 0 };
    g_autoptr(QDict) result = NULL;
    int32_t rc;

    if (!target || !*target) {
        error_setg(errp, "hlog query requires 'target'");
        return;
    }
    rc = hlog_ops.query_execute(target, query_args ? query_args : "", hlog_qemu_query_write, &output);
    if (rc != HLOG_OK) {
        error_setg(errp, "hlog query '%s' failed: %s", target, hlog_last_error());
        g_free(output.data);
        return;
    }

    result = qdict_new();
    qdict_put_str(result, "action", "query");
    qdict_put_str(result, "target", target);
    qdict_put_str(result, "output", output.data ? output.data : "");
    g_free(output.data);
    *ret = QOBJECT(g_steal_pointer(&result));
}

void qemu_hlog_qmp_handler(QDict *args, QObject **ret, Error **errp)
{
    const char *action;

    if (!hlog_ready) {
        error_setg(errp, "hlog provider is not loaded");
        return;
    }
    if (!args) {
        error_setg(errp, "hlog received NULL arguments");
        return;
    }
    if (!hlog_qmp_validate_args(args, errp)) {
        return;
    }

    action = qdict_get_try_str(args, "action");
    if (!action || !*action || !g_ascii_strcasecmp(action, "show")) {
        hlog_qmp_show(ret, errp);
    } else if (!g_ascii_strcasecmp(action, "set")) {
        hlog_qmp_set(args, ret, errp);
    } else if (!g_ascii_strcasecmp(action, "flush")) {
        hlog_qmp_flush(ret, errp);
    } else if (!g_ascii_strcasecmp(action, "console")) {
        hlog_qmp_console(args, ret, errp);
    } else if (!g_ascii_strcasecmp(action, "query-list")) {
        hlog_qmp_query_list(args, ret, errp);
    } else if (!g_ascii_strcasecmp(action, "query-help")) {
        hlog_qmp_query_help(args, ret, errp);
    } else if (!g_ascii_strcasecmp(action, "query")) {
        hlog_qmp_query(args, ret, errp);
    } else {
        error_setg(errp, "unknown hlog action '%s'; expected show/set/flush/console/query-list/query-help/query", action);
    }
}

static bool hlog_register_qmp(void)
{
    const QmpCommand *existing;

    if (hlog_qmp_registered) {
        return true;
    }

    existing = qmp_find_command(&qmp_commands, "hlog");
    if (existing) {
        hlog_early_report("cannot register QMP command 'hlog': command already exists");
        return false;
    }

    qmp_register_command(&qmp_commands, "hlog", qemu_hlog_qmp_handler, 0, 0);
    hlog_qmp_registered = true;
    return true;
}

bool qemu_hlog_control_available(void)
{
    return hlog_ready && hlog_qmp_registered;
}

#define RESOLVE_HLOG_SYMBOL(module, field, symbol_name)                                         \
    do {                                                                                        \
        if (!g_module_symbol((module), (symbol_name), (gpointer *)&hlog_ops.field)) {           \
            hlog_early_report("missing symbol '%s': %s", (symbol_name), g_module_error());     \
            goto fail;                                                                          \
        }                                                                                       \
    } while (0)

static bool hlog_resolve_runtime_symbols(GModule *module)
{
    RESOLVE_HLOG_SYMBOL(module, last_error, "hlog_last_error");
    RESOLVE_HLOG_SYMBOL(module, init_default, "hlog_init_default");
    RESOLVE_HLOG_SYMBOL(module, set_level_rule, "hlog_set_level_rule");
    RESOLVE_HLOG_SYMBOL(module, module_count, "hlog_module_count");
    RESOLVE_HLOG_SYMBOL(module, module_name_at, "hlog_module_name_at");
    RESOLVE_HLOG_SYMBOL(module, module_logger_count, "hlog_module_logger_count");
    RESOLVE_HLOG_SYMBOL(module, module_logger_at, "hlog_module_logger_at");
    RESOLVE_HLOG_SYMBOL(module, logger_module_name, "hlog_logger_module_name");
    RESOLVE_HLOG_SYMBOL(module, logger_display_name, "hlog_logger_display_name");
    RESOLVE_HLOG_SYMBOL(module, logger_level, "hlog_logger_level");
    RESOLVE_HLOG_SYMBOL(module, logger_set_level, "hlog_logger_set_level");
    RESOLVE_HLOG_SYMBOL(module, set_module_level, "hlog_set_module_level");
    RESOLVE_HLOG_SYMBOL(module, set_all_levels, "hlog_set_all_levels");
    RESOLVE_HLOG_SYMBOL(module, flush, "hlog_flush");
    RESOLVE_HLOG_SYMBOL(module, file_path, "hlog_file_path");
    RESOLVE_HLOG_SYMBOL(module, query_count, "hlog_query_count");
    RESOLVE_HLOG_SYMBOL(module, query_target_at, "hlog_query_target_at");
    RESOLVE_HLOG_SYMBOL(module, query_description_at, "hlog_query_description_at");
    RESOLVE_HLOG_SYMBOL(module, query_usage_at, "hlog_query_usage_at");
    RESOLVE_HLOG_SYMBOL(module, query_execute, "hlog_query_execute");
    /* Additive ABI v3 extension: old runtimes retain show/set/query support. */
    if (!g_module_symbol(module, "hlog_set_console_enabled",
                         (gpointer *)&hlog_ops.set_console_enabled) ||
        !g_module_symbol(module, "hlog_console_enabled",
                         (gpointer *)&hlog_ops.console_enabled)) {
        hlog_ops.set_console_enabled = NULL;
        hlog_ops.console_enabled = NULL;
    }
    return true;

fail:
    memset(&hlog_ops, 0, sizeof(hlog_ops));
    return false;
}

int qemu_hlog_preload(const char *path)
{
    GModule *module;
    uint32_t runtime_abi;
    int32_t rc;

    if (!path || !*path) {
        return 0;
    }
    if (hlog_ready) {
        return 0;
    }
    if (!g_module_supported()) {
        hlog_early_report("GModule is not supported on this platform");
        return -1;
    }

    /* No G_MODULE_BIND_LOCAL: model DSOs must see the runtime globally. */
    module = g_module_open(path, 0);
    if (!module) {
        hlog_early_report("failed to preload '%s': %s", path, g_module_error());
        return -1;
    }

    memset(&hlog_ops, 0, sizeof(hlog_ops));
    if (!g_module_symbol(module, "hlog_abi_version", (gpointer *)&hlog_ops.abi_version)) {
        hlog_early_report("missing symbol 'hlog_abi_version': %s", g_module_error());
        g_module_close(module);
        return -1;
    }

    runtime_abi = hlog_ops.abi_version();
    if (runtime_abi != HLOG_EXPECTED_ABI_VERSION) {
        hlog_early_report("ABI mismatch: QEMU expects %u, runtime provides %u", HLOG_EXPECTED_ABI_VERSION,
                          runtime_abi);
        memset(&hlog_ops, 0, sizeof(hlog_ops));
        g_module_close(module);
        return -1;
    }

    if (!hlog_resolve_runtime_symbols(module)) {
        g_module_close(module);
        return -1;
    }

    /* The ctor supplies fallback initialization; this makes the contract explicit. */
    rc = hlog_ops.init_default();
    if (rc != HLOG_OK) {
        hlog_early_report("initialization failed: %s", hlog_last_error());
        memset(&hlog_ops, 0, sizeof(hlog_ops));
        g_module_close(module);
        return -1;
    }

    g_module_make_resident(module);
    hlog_module = module;
    hlog_ready = true;

    if (!hlog_register_qmp()) {
        /* Keep the resident runtime available to model DSOs; only control is unavailable. */
        return -1;
    }

    hlog_early_report("loaded '%s'; QMP command hlog registered", path);
    return 0;
}

void qemu_hlog_preload_from_env(void)
{
    const char *path = getenv("HLOG_PATH");

    if (!path || !*path) {
        return;
    }

    /* Optional feature: report the failure, but do not terminate QEMU. */
    (void)qemu_hlog_preload(path);
}

typedef struct HLogRuleApplyContext {
    size_t applied;
} HLogRuleApplyContext;

static int hlog_apply_config_rule(void *opaque, QemuOpts *opts, Error **errp)
{
    HLogRuleApplyContext *context = opaque;
    const char *target = qemu_opts_id(opts);
    const char *level_text = qemu_opt_get(opts, "level");
    uint32_t level;
    int32_t rc;

    if (!target || !*target) {
        error_setg(errp, "[hlog] section requires a target id, for example [hlog \"bmu\"]");
        return -1;
    }
    if (!level_text || !*level_text) {
        error_setg(errp, "[hlog \"%s\"] requires 'level'", target);
        return -1;
    }
    if (!hlog_parse_level(level_text, &level)) {
        error_setg(errp, "invalid hlog level '%s' for target '%s'", level_text, target);
        return -1;
    }

    rc = hlog_ops.set_level_rule(target, level);
    if (rc != HLOG_OK) {
        error_setg(errp, "failed to set hlog rule '%s=%s': %s", target, level_text, hlog_last_error());
        return -1;
    }

    context->applied++;
    return 0;
}

bool qemu_hlog_apply_config_rules(Error **errp)
{
    HLogRuleApplyContext context = { 0 };

    if (QTAILQ_EMPTY(&hlog_rule_opts.head)) {
        return true;
    }

    /*
     * hlog is optional. load_object_plugins() performs the early environment
     * preload before model DSOs are opened. If it was skipped or failed, the
     * presence of hlog rules must not prevent normal QEMU startup.
     */
    if (!hlog_ready) {
        hlog_early_report("runtime is not loaded; ignoring -readconfig level rules");
        return true;
    }

    if (qemu_opts_foreach(&hlog_rule_opts, hlog_apply_config_rule, &context, errp) != 0) {
        return false;
    }

    if (context.applied) {
        hlog_early_report("applied %zu startup level rule(s) from -readconfig", context.applied);
    }
    return true;
}
