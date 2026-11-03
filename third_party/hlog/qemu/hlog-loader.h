/*
 * Optional hlog preload helper.
 *
 * Copyright (c) 2026-, Huawei
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QEMU_HLOG_LOADER_H
#define QEMU_HLOG_LOADER_H

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qobject.h"

/*
 * Optionally preload the process-global hlog runtime.
 *
 * path == NULL or "" means skip the optional feature and return 0. QEMU loads
 * the runtime through GModule, validates ABI v3, resolves the control symbols,
 * calls hlog_init_default(), registers hlog, and keeps the module resident
 * for the whole QEMU process lifetime.
 *
 * QEMU has no build-time dependency on libhlog.so and does not include hlog.h.
 */
int qemu_hlog_preload(const char *path);

/* Legacy convenience wrapper; HLOG_PATH absent or empty is a silent no-op. */
void qemu_hlog_preload_from_env(void);

/*
 * Apply level rules from QEMU's existing -readconfig mechanism.
 *
 * The loader registers this repeatable QemuOpts group through opts_init():
 *
 *     [hlog "bmu"]
 *     level = "info"
 *
 * Call once after QEMU has parsed all command-line/-readconfig options. The
 * runtime must already have been preloaded from load_object_plugins() through
 * qemu_hlog_preload_from_env(). Existing and future loggers receive the rules.
 *
 * If the optional runtime is not loaded, rules are ignored and QEMU continues.
 */
bool qemu_hlog_apply_config_rules(Error **errp);

/*
 * True only when libhlog is loaded and the hlog QMP command is registered.
 * This does not indicate whether any module query has been registered.
 */
bool qemu_hlog_control_available(void);

/*
 * Exact handler registered as hlog. The HMP hlog command calls the same
 * handler, so HMP remains a human-readable wrapper around the QMP implementation.
 * Query actions accept the optional string argument named "args".
 * query-list accepts a module, instance, or full query filter in "target".
 */
void qemu_hlog_qmp_handler(QDict *args, QObject **ret, Error **errp);

#endif /* QEMU_HLOG_LOADER_H */
