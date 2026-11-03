#include "qemu/osdep.h"
#include "qemu/hlog-loader.h"
#include "monitor/monitor.h"
#include "qemu/hmp-hlog.c.inc"

void monitor_printf(Monitor *mon, const char *format, ...)
{
    va_list args;
    size_t used = strlen(mon->text);
    va_start(args, format);
    vsnprintf(mon->text + used, sizeof(mon->text) - used, format, args);
    va_end(args);
}
void hmp_handle_error(Monitor *mon, Error *err)
{
    monitor_printf(mon, "ERROR: %s", err->msg);
    error_free(err);
}
static void call(Monitor *mon, const char *action, const char *target, const char *value)
{
    QDict *args = qdict_new();
    mon->text[0] = 0;
    qdict_put_str(args, "action", action);
    if (target) { qdict_put_str(args, "target", target); }
    if (value) { qdict_put_str(args, "value", value); }
    hmp_hlog(mon, args);
    qdict_destroy(args);
}
int main(int argc, char **argv)
{
    Monitor mon = { 0 };
    bool old;
    assert(argc == 3);
    old = !strcmp(argv[2], "old");
    assert(qemu_hlog_preload(argv[1]) == 0);
    assert(qemu_hlog_control_available());
    call(&mon, "console", NULL, NULL);
    if (old) {
        assert(strstr(mon.text, "requires runtime 3.1.0"));
    } else {
        assert(strstr(mon.text, "mirror: off"));
        call(&mon, "console", "on", NULL);
        assert(strstr(mon.text, "mirror: on"));
        call(&mon, "console", "off", "extra");
        assert(strstr(mon.text, "ERROR:"));
        call(&mon, "console", "show", NULL);
        assert(strstr(mon.text, "mirror: on"));
        call(&mon, "console", "off", NULL);
        assert(strstr(mon.text, "mirror: off"));
    }
    call(&mon, "show", NULL, NULL);
    assert(strstr(mon.text, "LOGGER"));
    assert(strstr(mon.text, "bmu/1"));
    if (old) { assert(strstr(mon.text, "not supported")); }
    call(&mon, "query-list", "bmu/0", NULL);
    assert(strstr(mon.text, "Total: 3 queries"));
    call(&mon, "query-help", "bmu/0/map-table", NULL);
    assert(strstr(mon.text, "vf=<u32>"));
    call(&mon, "query", "bmu/0/map-table", "vf=3 l_pool=1");
    assert(strstr(mon.text, "vf=3 l_pool=1"));
    call(&mon, "set", "bmu", "debug");
    assert(strstr(mon.text, "bmu: debug"));
    call(&mon, "flush", NULL, NULL);
    assert(strstr(mon.text, "flushed"));
    puts("HMP wrapper tests passed (mock runtime and QEMU)");
    return 0;
}
