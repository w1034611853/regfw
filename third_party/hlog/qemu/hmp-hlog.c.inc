/* Include from monitor/hmp-cmds.c IN PLACE OF the old hmp_hlog definition.
 * HMP output stays on the requesting monitor. Mirrored log output uses stderr.
 */
#include "qemu/hlog-loader.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"

void hmp_hlog(Monitor *mon, const QDict *qdict)
{
    const char *action = qdict_get_try_str(qdict, "action");
    const char *target = qdict_get_try_str(qdict, "target");
    const char *value = qdict_get_try_str(qdict, "value");
    QDict *args = qdict_new();
    QObject *ret = NULL;
    QDict *result;
    Error *err = NULL;
    const QListEntry *item;
    size_t count = 0;

    action = action ? action : "show";
    qdict_put_str(args, "action", action);
    if (target) {
        qdict_put_str(args, "target", target);
    }
    if (value) {
        qdict_put_str(args, !g_ascii_strcasecmp(action, "set") ? "level" : "args", value);
    }
    qemu_hlog_qmp_handler(args, &ret, &err);
    qobject_unref(QOBJECT(args));
    if (err) {
        hmp_handle_error(mon, err);
        qobject_unref(ret);
        return;
    }
    result = qobject_to(QDict, ret);
    if (!g_ascii_strcasecmp(action, "console")) {
        monitor_printf(mon, "HLOG console mirror: %s (stderr)\n",
                       qdict_get_bool(result, "console_enabled") ? "on" : "off");
    } else if (!g_ascii_strcasecmp(action, "query")) {
        const char *output = qdict_get_str(result, "output");
        size_t length = strlen(output);

        monitor_printf(mon, "%s", output);
        if (length && output[length - 1] != '\n') {
            monitor_printf(mon, "\n");
        }
    } else if (!g_ascii_strcasecmp(action, "query-help")) {
        QDict *query = qdict_get_qdict(result, "query");

        monitor_printf(mon, "%s\n%s\n", qdict_get_str(query, "description"),
                       qdict_get_str(query, "usage"));
    } else if (!g_ascii_strcasecmp(action, "query-list")) {
        QList *queries = qdict_get_qlist(result, "queries");

        monitor_printf(mon, "%-36s  %s\n", "QUERY", "DESCRIPTION");
        for (item = qlist_first(queries); item; item = qlist_next(item)) {
            QDict *query = qobject_to(QDict, qlist_entry_obj(item));

            monitor_printf(mon, "%-36s  %s\n", qdict_get_str(query, "target"),
                           qdict_get_str(query, "description"));
            count++;
        }
        monitor_printf(mon, "Total: %zu queries\n", count);
    } else if (!g_ascii_strcasecmp(action, "set")) {
        monitor_printf(mon, "HLOG %s: %s\n", qdict_get_str(result, "target"),
                       qdict_get_str(result, "level"));
    } else if (!g_ascii_strcasecmp(action, "flush")) {
        monitor_printf(mon, "HLOG flushed\n");
    } else {
        QList *loggers = qdict_get_qlist(result, "loggers");

        monitor_printf(mon, "HLOG file: %s\n", qdict_get_str(result, "file"));
        monitor_printf(mon, "HLOG console mirror: %s\n",
                       qdict_get_bool(result, "console_supported") ?
                       (qdict_get_bool(result, "console_enabled") ? "on (stderr)" : "off") :
                       "not supported by loaded runtime");
        monitor_printf(mon, "%-28s  %-16s  %s\n", "LOGGER", "MODULE", "LEVEL");
        for (item = qlist_first(loggers); item; item = qlist_next(item)) {
            QDict *logger = qobject_to(QDict, qlist_entry_obj(item));

            monitor_printf(mon, "%-28s  %-16s  %s\n", qdict_get_str(logger, "name"),
                           qdict_get_str(logger, "module"), qdict_get_str(logger, "level"));
        }
    }
    qobject_unref(ret);
}
