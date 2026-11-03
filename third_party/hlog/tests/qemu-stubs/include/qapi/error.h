#ifndef TEST_ERROR_H
#define TEST_ERROR_H

#include "qemu/osdep.h"

typedef struct Error {
    char *msg;
} Error;

void error_setg(Error **errp, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void error_setg_errno(Error **errp, int os_error, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
void error_free(Error *error);

#endif
