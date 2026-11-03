#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct HLogLogger {
    char module[32];
    char display[64];
    uint32_t fallback_level;
    uint32_t level;
} HLogLogger;

typedef struct FakeRule {
    char target[64];
    uint32_t level;
} FakeRule;

typedef struct FakeRules {
    bool has_all;
    uint32_t all;
    FakeRule modules[16];
    size_t module_count;
    FakeRule loggers[16];
    size_t logger_count;
} FakeRules;

static HLogLogger loggers[8] = {
    { "bmu", "bmu/1", 2, 2 },
    { "bmu", "bmu/chip0", 3, 3 },
    { "crg", "crg/1", 4, 4 },
};
static size_t logger_count = 3;
static const char *modules[] = { "bmu", "crg" };
static char last_error[256];
static FakeRules active_rules;

#ifndef HLOG_TEST_OLD_RUNTIME
static bool console_enabled;
int32_t hlog_set_console_enabled(bool enabled)
{
    console_enabled = enabled;
    return 0;
}
bool hlog_console_enabled(void)
{
    return console_enabled;
}
#endif

static size_t copy_string(const char *source, char *buffer, size_t capacity)
{
    size_t required = strlen(source) + 1;

    if (!buffer || capacity == 0) {
        return required;
    }
    if (capacity < required) {
        return 0;
    }
    memcpy(buffer, source, required);
    return required;
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool parse_level(const char *text, uint32_t *level)
{
    static const char *names[] = { "trace", "debug", "info", "warn", "error", "critical", "off" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcasecmp(text, names[i])) {
            *level = i;
            return true;
        }
    }
    return false;
}

static int find_rule(const FakeRule *rules, size_t count, const char *target)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (!strcmp(rules[i].target, target)) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t effective_level(const FakeRules *rules, const HLogLogger *logger)
{
    int index = find_rule(rules->loggers, rules->logger_count, logger->display);

    if (index >= 0) {
        return rules->loggers[index].level;
    }
    index = find_rule(rules->modules, rules->module_count, logger->module);
    if (index >= 0) {
        return rules->modules[index].level;
    }
    return rules->has_all ? rules->all : logger->fallback_level;
}

static void apply_rules(void)
{
    size_t i;

    for (i = 0; i < logger_count; i++) {
        loggers[i].level = effective_level(&active_rules, &loggers[i]);
    }
}

static bool append_rule(FakeRule *rules, size_t *count, const char *target, uint32_t level)
{
    if (find_rule(rules, *count, target) >= 0 || *count >= 16) {
        return false;
    }
    snprintf(rules[*count].target, sizeof(rules[*count].target), "%s", target);
    rules[*count].level = level;
    (*count)++;
    return true;
}

uint32_t hlog_abi_version(void)
{
    return 3;
}

typedef int32_t (*HLogQueryWriteFn)(void *opaque, const uint8_t *data, size_t length);

static const char *query_targets[] = {
    "bmu/0/map-table",
    "bmu/0/status",
    "bmu/1/map-table",
    "bmu/10/map-table",
    "bmu2/0/map-table",
    "crg/0/status",
    "bmu/0/map-table-extra",
};

size_t hlog_query_count(void)
{
    return sizeof(query_targets) / sizeof(query_targets[0]);
}

size_t hlog_query_target_at(size_t index, char *buffer, size_t capacity)
{
    return index < hlog_query_count()
        ? copy_string(query_targets[index], buffer, capacity) : 0;
}

size_t hlog_query_description_at(size_t index, char *buffer, size_t capacity)
{
    return index < hlog_query_count()
        ? copy_string("Show module state", buffer, capacity) : 0;
}

size_t hlog_query_usage_at(size_t index, char *buffer, size_t capacity)
{
    if (index == 0) {
        return copy_string("bmu/0/map-table vf=<u32> [l_pool=<u32>]",
                           buffer, capacity);
    }
    return hlog_query_target_at(index, buffer, capacity);
}

int32_t hlog_query_execute(const char *target, const char *args, HLogQueryWriteFn write, void *opaque)
{
    char output[256];
    int length;

    if (strcmp(target, "bmu/0/map-table")) {
        snprintf(last_error, sizeof(last_error), "query '%s' not found", target);
        return -3;
    }
    length = snprintf(output, sizeof(output), "target=%s args=%s\n", target, args ? args : "");
    return write(opaque, (const uint8_t *)output, (size_t)length);
}

const char *hlog_last_error(void)
{
    return last_error;
}

int32_t hlog_init_default(void)
{
    return 0;
}

int32_t hlog_load_level_config(const char *path)
{
    FakeRules rules = { 0 };
    char line[256];
    unsigned int line_number = 0;
    size_t rule_count = 0;
    FILE *stream = fopen(path, "r");

    if (!stream) {
        snprintf(last_error, sizeof(last_error), "failed to read hlog level config '%s'", path);
        return -3;
    }

    while (fgets(line, sizeof(line), stream)) {
        char *comment;
        char *separator;
        char *target;
        char *level_text;
        uint32_t level;

        line_number++;
        comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }
        target = trim(line);
        if (!*target) {
            continue;
        }
        separator = strchr(target, '=');
        if (!separator || strchr(separator + 1, '=')) {
            snprintf(last_error, sizeof(last_error), "%s:%u: expected 'target = level'", path, line_number);
            fclose(stream);
            return -1;
        }

        *separator = '\0';
        level_text = trim(separator + 1);
        target = trim(target);
        if (!parse_level(level_text, &level)) {
            snprintf(last_error, sizeof(last_error), "%s:%u: invalid level '%s'", path, line_number, level_text);
            fclose(stream);
            return -1;
        }

        if (!strcasecmp(target, "all")) {
            if (rules.has_all) {
                snprintf(last_error, sizeof(last_error), "%s:%u: duplicate log target 'all'", path, line_number);
                fclose(stream);
                return -1;
            }
            rules.has_all = true;
            rules.all = level;
        } else if (strchr(target, '/')) {
            if (!append_rule(rules.loggers, &rules.logger_count, target, level)) {
                snprintf(last_error, sizeof(last_error), "%s:%u: duplicate log target '%s'", path, line_number,
                         target);
                fclose(stream);
                return -1;
            }
        } else if (!append_rule(rules.modules, &rules.module_count, target, level)) {
            snprintf(last_error, sizeof(last_error), "%s:%u: duplicate log target '%s'", path, line_number,
                     target);
            fclose(stream);
            return -1;
        }
        rule_count++;
    }
    fclose(stream);

    if (!rule_count) {
        snprintf(last_error, sizeof(last_error), "hlog level config '%s' contains no rules", path);
        return -1;
    }

    active_rules = rules;
    apply_rules();
    last_error[0] = '\0';
    return 0;
}

int32_t hlog_set_level_rule(const char *target, uint32_t level)
{
    FakeRule *rules;
    size_t *count;
    int index;

    if (!target || !*target || level > 6) {
        snprintf(last_error, sizeof(last_error), "invalid target or level");
        return -1;
    }
    if (!strcasecmp(target, "all")) {
        active_rules.has_all = true;
        active_rules.all = level;
    } else {
        if (strchr(target, '/')) {
            rules = active_rules.loggers;
            count = &active_rules.logger_count;
        } else {
            rules = active_rules.modules;
            count = &active_rules.module_count;
        }
        index = find_rule(rules, *count, target);
        if (index >= 0) {
            rules[index].level = level;
        } else if (!append_rule(rules, count, target, level)) {
            snprintf(last_error, sizeof(last_error), "too many hlog rules");
            return -2;
        }
    }
    apply_rules();
    last_error[0] = '\0';
    return 0;
}

HLogLogger *fake_hlog_register_logger(const char *module, const char *display, uint32_t fallback_level)
{
    HLogLogger *logger;

    if (logger_count >= sizeof(loggers) / sizeof(loggers[0])) {
        return NULL;
    }
    logger = &loggers[logger_count++];
    snprintf(logger->module, sizeof(logger->module), "%s", module);
    snprintf(logger->display, sizeof(logger->display), "%s", display);
    logger->fallback_level = fallback_level;
    logger->level = effective_level(&active_rules, logger);
    return logger;
}

size_t hlog_module_count(void)
{
    return sizeof(modules) / sizeof(modules[0]);
}

size_t hlog_module_name_at(size_t index, char *buffer, size_t capacity)
{
    return index < hlog_module_count() ? copy_string(modules[index], buffer, capacity) : 0;
}

size_t hlog_module_logger_count(const char *module_name)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < logger_count; i++) {
        count += !strcmp(loggers[i].module, module_name);
    }
    return count;
}

HLogLogger *hlog_module_logger_at(const char *module_name, size_t index)
{
    size_t i;

    for (i = 0; i < logger_count; i++) {
        if (!strcmp(loggers[i].module, module_name) && index-- == 0) {
            return &loggers[i];
        }
    }
    return NULL;
}

size_t hlog_logger_module_name(const HLogLogger *logger, char *buffer, size_t capacity)
{
    return logger ? copy_string(logger->module, buffer, capacity) : 0;
}

size_t hlog_logger_display_name(const HLogLogger *logger, char *buffer, size_t capacity)
{
    return logger ? copy_string(logger->display, buffer, capacity) : 0;
}

uint32_t hlog_logger_level(const HLogLogger *logger)
{
    return logger ? logger->level : 6;
}

int32_t hlog_logger_set_level(HLogLogger *logger, uint32_t level)
{
    int index;

    if (!logger || level > 6) {
        snprintf(last_error, sizeof(last_error), "invalid logger or level");
        return -1;
    }
    index = find_rule(active_rules.loggers, active_rules.logger_count, logger->display);
    if (index < 0) {
        append_rule(active_rules.loggers, &active_rules.logger_count, logger->display, level);
    } else {
        active_rules.loggers[index].level = level;
    }
    logger->level = level;
    return 0;
}

int32_t hlog_set_module_level(const char *module_name, uint32_t level)
{
    size_t count = 0;
    size_t i;
    int index;

    for (i = 0; i < logger_count; i++) {
        if (!strcmp(loggers[i].module, module_name)) {
            loggers[i].level = level;
            count++;
        }
    }
    if (!count) {
        snprintf(last_error, sizeof(last_error), "module not found");
        return -3;
    }

    index = find_rule(active_rules.modules, active_rules.module_count, module_name);
    if (index < 0) {
        append_rule(active_rules.modules, &active_rules.module_count, module_name, level);
    } else {
        active_rules.modules[index].level = level;
    }
    for (i = 0; i < active_rules.logger_count;) {
        size_t module_length = strlen(module_name);

        if (!strncmp(active_rules.loggers[i].target, module_name, module_length)
            && active_rules.loggers[i].target[module_length] == '/') {
            active_rules.loggers[i] = active_rules.loggers[--active_rules.logger_count];
        } else {
            i++;
        }
    }
    return 0;
}

int32_t hlog_set_all_levels(uint32_t level)
{
    size_t i;

    active_rules.has_all = true;
    active_rules.all = level;
    active_rules.module_count = 0;
    active_rules.logger_count = 0;
    for (i = 0; i < logger_count; i++) {
        loggers[i].level = level;
    }
    return 0;
}

int32_t hlog_flush(void)
{
    return 0;
}

size_t hlog_file_path(char *buffer, size_t capacity)
{
    return copy_string("/tmp/qemu-device.log", buffer, capacity);
}
