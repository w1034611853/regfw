#ifndef TEST_HMP_H
#define TEST_HMP_H
#include "monitor/monitor.h"
#include "qapi/error.h"
void hmp_handle_error(Monitor *mon, Error *err);
#endif
