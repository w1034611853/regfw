#!/usr/bin/env bash
set -euo pipefail

LIB="${1:-}"
if [[ -z "${LIB}" ]]; then
    echo "usage: $0 /path/to/libhlog.so.1" >&2
    exit 2
fi

readelf -d "${LIB}" | grep -E 'SONAME.*libhlog\.so\.1' >/dev/null || {
    echo "ERROR: expected SONAME libhlog.so.1" >&2
    exit 1
}

for sym in \
    hlog_abi_version \
    hlog_init_default \
    hlog_init \
    hlog_load_level_config \
    hlog_set_level_rule \
    hlog_query_register \
    hlog_query_unregister \
    hlog_query_count \
    hlog_query_target_at \
    hlog_query_description_at \
    hlog_query_usage_at \
    hlog_query_execute \
    hlog_query_set_error \
    hlog_register_logger \
    hlog_enabled \
    hlog_write \
    hlog_hexdump \
    hlog_hexdump_ex \
    hlog_set_console_enabled \
    hlog_console_enabled \
    hlog_console_write \
    hlog_console_hexdump \
    hlog_console_hexdump_ex \
    hlog_set_module_level \
    hlog_flush; do
    nm -D --defined-only "${LIB}" | grep -Eq "[[:space:]]${sym}$" || {
        echo "ERROR: missing exported symbol ${sym}" >&2
        exit 1
    }
done

abi="$({ python3 - "${LIB}" <<'PY'
import ctypes
import sys

runtime = ctypes.CDLL(sys.argv[1])
runtime.hlog_abi_version.restype = ctypes.c_uint32
print(runtime.hlog_abi_version())
PY
} 2>/dev/null)"
if [[ "${abi}" != "3" ]]; then
    echo "ERROR: expected hlog ABI 3, got ${abi:-unknown}" >&2
    exit 1
fi

echo "hlog runtime ABI v3 looks OK: ${LIB}"
