#include "qemu/osdep.h"

#include <assert.h>
#include <dlfcn.h>

#include "monitor/monitor-internal.h"
#include "qemu/hlog-loader.h"
#include "qobject/qlist.h"
#include "system/system.h"

static QDict *new_action(const char *action)
{
    QDict *args = qdict_new();

    if (action) {
        qdict_put_str(args, "action", action);
    }
    return args;
}

static QObject *execute(QDict *args)
{
    QObject *result = NULL;
    Error *error = NULL;

    qemu_hlog_qmp_handler(args, &result, &error);
    assert(error == NULL);
    qdict_destroy(args);
    return result;
}

static void expect_error(QDict *args, const char *substring)
{
    QObject *result = NULL;
    Error *error = NULL;

    qemu_hlog_qmp_handler(args, &result, &error);
    assert(result == NULL);
    assert(error != NULL);
    assert(strstr(error->msg, substring) != NULL);
    error_free(error);
    qdict_destroy(args);
}

static void expect_logger_level(const char *name, const char *level)
{
    QDict *result = (QDict *)execute(new_action("show"));
    QList *loggers = (QList *)qdict_get(result, "loggers");
    size_t i;
    bool found = false;

    for (i = 0; i < qlist_size(loggers); i++) {
        QDict *logger = (QDict *)qlist_get(loggers, i);

        if (!strcmp(qdict_get_try_str(logger, "name"), name)) {
            assert(!strcmp(qdict_get_try_str(logger, "level"), level));
            found = true;
        }
    }
    assert(found);
    qobject_unref(QOBJECT(result));
}

static void test_query_filters(void)
{
    static const char *targets[] = {
        "bmu/0/map-table", "bmu/0/status", "bmu/1/map-table",
        "bmu/10/map-table", "bmu2/0/map-table", "crg/0/status",
        "bmu/0/map-table-extra",
    };
    static const struct {
        const char *filter;
        unsigned int mask;
    } cases[] = {
        { NULL, 127 }, { "", 127 }, { "bmu", 79 }, { "bmu/", 79 },
        { "bmu/0", 67 }, { "bmu/0/", 67 }, { "bmu/1", 4 },
        { "bmu/10", 8 }, { "bmu2", 16 }, { "crg", 32 },
        { "bmu/0/map-table", 1 }, { "bmu/0/map-table-extra", 64 },
        { "bm", 0 }, { "BMU", 0 }, { "missing", 0 }, { "/", 0 },
        { "bmu/0/map-table-too-long", 0 }, { "bmu/0/map", 0 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        QDict *args = new_action("query-list");
        QDict *result;
        QList *queries;
        size_t j;
        size_t found = 0;

        if (cases[i].filter) {
            qdict_put_str(args, "target", cases[i].filter);
        }
        result = (QDict *)execute(args);
        queries = (QList *)qdict_get(result, "queries");
        assert(!strcmp(qdict_get_try_str(result, "action"), "query-list"));
        for (j = 0; j < sizeof(targets) / sizeof(targets[0]); j++) {
            QDict *query;

            if (!(cases[i].mask & (1u << j))) {
                continue;
            }
            assert(found < qlist_size(queries));
            query = (QDict *)qlist_get(queries, found++);
            assert(!strcmp(qdict_get_try_str(query, "target"), targets[j]));
        }
        assert(found == qlist_size(queries));
        qobject_unref(QOBJECT(result));
    }
    puts("18 query-list filter cases passed");
}

int main(int argc, char **argv)
{
    typedef void *(*FakeRegisterLoggerFn)(const char *module, const char *display, uint32_t fallback_level);

    const QmpCommand *command;
    FakeRegisterLoggerFn register_logger;
    QDict *args;
    QDict *result;

    assert(argc == 3);
    unsetenv("HLOG_PATH");
    qemu_hlog_preload_from_env();
    assert(!qemu_hlog_control_available());
    assert(qemu_hlog_preload(NULL) == 0);

    {
        Error *error = NULL;

        assert(test_qemu_read_config(argv[2], &error));
        assert(error == NULL);
        assert(qemu_hlog_apply_config_rules(&error));
        assert(error == NULL);
        assert(!qemu_hlog_control_available());
    }

    assert(setenv("HLOG_PATH", argv[1], 1) == 0);
    qemu_hlog_preload_from_env();
    {
        Error *error = NULL;

        assert(qemu_hlog_apply_config_rules(&error));
        assert(error == NULL);
    }
    assert(qemu_hlog_control_available());
    assert(qemu_hlog_preload(argv[1]) == 0);

    command = qmp_find_command(&qmp_commands, "hlog");
    assert(command != NULL);
    assert(command->handler == qemu_hlog_qmp_handler);

    expect_logger_level("bmu/1", "trace");
    expect_logger_level("bmu/chip0", "info");
    expect_logger_level("crg/1", "off");

    register_logger = (FakeRegisterLoggerFn)dlsym(RTLD_DEFAULT, "fake_hlog_register_logger");
    assert(register_logger != NULL);
    assert(register_logger("bmu", "bmu/2", 4) != NULL);
    expect_logger_level("bmu/2", "info");

    result = (QDict *)execute(new_action(NULL));
    assert(qdict_get_bool(result, "loaded"));
    assert(!strcmp(qdict_get_try_str(result, "file"), "/tmp/qemu-device.log"));
    assert(qlist_size((QList *)qdict_get(result, "loggers")) == 4);
    qobject_unref(QOBJECT(result));

    args = new_action("set");
    qdict_put_str(args, "target", "all");
    qdict_put_str(args, "level", "info");
    qobject_unref(execute(args));
    expect_logger_level("crg/1", "info");
    assert(register_logger("crg", "crg/2", 4) != NULL);
    expect_logger_level("crg/2", "info");

    args = new_action("set");
    qdict_put_str(args, "target", "bmu");
    qdict_put_str(args, "level", "debug");
    qobject_unref(execute(args));
    expect_logger_level("bmu/chip0", "debug");
    assert(register_logger("bmu", "bmu/3", 4) != NULL);
    expect_logger_level("bmu/3", "debug");

    args = new_action("set");
    qdict_put_str(args, "target", "bmu/1");
    qdict_put_str(args, "level", "trace");
    qobject_unref(execute(args));
    expect_logger_level("bmu/1", "trace");

    result = (QDict *)execute(new_action("flush"));
    assert(qdict_get_bool(result, "flushed"));
    qobject_unref(QOBJECT(result));

    result = (QDict *)execute(new_action("query-list"));
    assert(qlist_size((QList *)qdict_get(result, "queries")) == 7);
    assert(!strcmp(qdict_get_try_str((QDict *)qlist_get((QList *)qdict_get(result, "queries"), 0), "target"),
                   "bmu/0/map-table"));
    qobject_unref(QOBJECT(result));

    test_query_filters();

    result = (QDict *)execute(new_action("console"));
    assert(!qdict_get_bool(result, "console_enabled"));
    qobject_unref(QOBJECT(result));

    args = new_action("console");
    qdict_put_str(args, "target", "on");
    result = (QDict *)execute(args);
    assert(qdict_get_bool(result, "console_enabled"));
    assert(!strcmp(qdict_get_try_str(result, "destination"), "stderr"));
    qobject_unref(QOBJECT(result));

    result = (QDict *)execute(new_action("show"));
    assert(qdict_get_bool(result, "console_supported"));
    assert(qdict_get_bool(result, "console_enabled"));
    qobject_unref(QOBJECT(result));

    args = new_action("console");
    qdict_put_str(args, "target", "bad");
    expect_error(args, "on/off/show");
    args = new_action("console");
    qdict_put_str(args, "target", "off");
    qdict_put_str(args, "args", "surplus");
    expect_error(args, "expects only");
    result = (QDict *)execute(new_action("console"));
    assert(qdict_get_bool(result, "console_enabled"));
    qobject_unref(QOBJECT(result));

    args = new_action("console");
    qdict_put_str(args, "target", "off");
    result = (QDict *)execute(args);
    assert(!qdict_get_bool(result, "console_enabled"));
    qobject_unref(QOBJECT(result));

    args = new_action("query-help");
    qdict_put_str(args, "target", "bmu/0/map-table");
    result = (QDict *)execute(args);
    assert(strstr(qdict_get_try_str((QDict *)qdict_get(result, "query"), "usage"), "vf=<u32>") != NULL);
    qobject_unref(QOBJECT(result));

    args = new_action("query");
    qdict_put_str(args, "target", "bmu/0/map-table");
    qdict_put_str(args, "args", "vf=3 l_pool=1");
    result = (QDict *)execute(args);
    assert(strstr(qdict_get_try_str(result, "output"), "vf=3 l_pool=1") != NULL);
    qobject_unref(QOBJECT(result));

    args = new_action("set");
    qdict_put_str(args, "target", "missing");
    qdict_put_str(args, "level", "debug");
    expect_error(args, "module not found");

    args = new_action("show");
    qdict_put_str(args, "unexpected", "value");
    expect_error(args, "unexpected argument");

    puts("QEMU hlog -readconfig loader tests passed");
    return 0;
}
