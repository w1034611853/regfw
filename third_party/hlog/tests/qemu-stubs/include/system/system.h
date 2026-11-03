#ifndef TEST_SYSTEM_SYSTEM_H
#define TEST_SYSTEM_SYSTEM_H

#include "qemu/notify.h"
#include "qapi/error.h"

void qemu_add_machine_init_done_notifier(Notifier *notifier);
void test_qemu_machine_init_done(void);
bool test_qemu_read_config(const char *path, Error **errp);

#endif
