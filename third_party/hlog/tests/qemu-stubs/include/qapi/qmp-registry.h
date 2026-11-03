#ifndef TEST_QMP_REGISTRY_H
#define TEST_QMP_REGISTRY_H

#include "qapi/error.h"
#include "qobject/qdict.h"

typedef void (*QmpCommandFunc)(QDict *args, QObject **ret, Error **errp);

typedef struct QmpCommand {
    char *name;
    QmpCommandFunc handler;
    struct QmpCommand *next;
} QmpCommand;

typedef struct QmpCommandList {
    QmpCommand *head;
} QmpCommandList;

const QmpCommand *qmp_find_command(const QmpCommandList *commands, const char *name);
void qmp_register_command(QmpCommandList *commands, const char *name, QmpCommandFunc handler, int options,
                          int features);

#endif
