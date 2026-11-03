#ifndef TEST_GMODULE_H
#define TEST_GMODULE_H

#include <stdbool.h>

typedef void *gpointer;
typedef struct GModule GModule;

bool g_module_supported(void);
GModule *g_module_open(const char *path, int flags);
bool g_module_close(GModule *module);
bool g_module_symbol(GModule *module, const char *symbol_name, gpointer *symbol);
const char *g_module_error(void);
void g_module_make_resident(GModule *module);

#endif
