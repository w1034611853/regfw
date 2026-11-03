#include "qemu/osdep.h"

#include <assert.h>

#include "qemu/hlog-loader.h"
#include "system/system.h"

int main(int argc, char **argv)
{
    Error *error = NULL;
    bool parsed;
    bool applied = false;

    assert(argc == 4);
    assert(setenv("HLOG_PATH", argv[1], 1) == 0);
    qemu_hlog_preload_from_env();
    parsed = test_qemu_read_config(argv[2], &error);
    if (parsed) {
        assert(error == NULL);
        applied = qemu_hlog_apply_config_rules(&error);
    }
    assert(!parsed || !applied);
    assert(error != NULL);
    assert(strstr(error->msg, argv[3]) != NULL);
    error_free(error);
    puts("QEMU hlog -readconfig error test passed");
    return 0;
}
