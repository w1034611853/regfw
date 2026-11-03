#ifndef TEST_MONITOR_H
#define TEST_MONITOR_H
#include "qemu/osdep.h"
typedef struct Monitor {
    char text[16384];
} Monitor;
void monitor_printf(Monitor *mon, const char *format, ...);
#endif
