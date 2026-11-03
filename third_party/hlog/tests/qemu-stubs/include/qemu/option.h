#ifndef TEST_QEMU_OPTION_H
#define TEST_QEMU_OPTION_H

#include "qapi/error.h"
#include "qemu/osdep.h"

typedef enum QemuOptType {
    QEMU_OPT_STRING,
} QemuOptType;

typedef struct QemuOptDesc {
    const char *name;
    QemuOptType type;
} QemuOptDesc;

typedef struct QemuOptValue {
    char *name;
    char *value;
    struct QemuOptValue *next;
} QemuOptValue;

typedef struct QemuOpts {
    char *id;
    QemuOptValue *values;
    QTAILQ_ENTRY(QemuOpts) next;
} QemuOpts;

typedef struct QemuOptsList {
    const char *name;
    const char *implied_opt_name;
    bool merge_lists;
    QTAILQ_HEAD(, QemuOpts) head;
    QemuOptDesc desc[16];
} QemuOptsList;

typedef int (*qemu_opts_loopfunc)(void *opaque, QemuOpts *opts, Error **errp);

QemuOpts *qemu_opts_find(QemuOptsList *list, const char *id);
const char *qemu_opts_id(QemuOpts *opts);
const char *qemu_opt_get(QemuOpts *opts, const char *name);
int qemu_opts_foreach(QemuOptsList *list, qemu_opts_loopfunc func, void *opaque, Error **errp);

#endif
