#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lib="${root}/third_party/hlog/lib/libhlog.so.1.0.0"
xz -dc "${lib}.xz" > "${lib}"
chmod 755 "${lib}"
if command -v git >/dev/null 2>&1; then
    actual="$(git hash-object "${lib}")"
    test "${actual}" = "7419d74dd3793bb38f0e3fe1317ccf3e07ba765a" || {
        echo "hlog library hash mismatch" >&2
        exit 1
    }
fi
echo "Restored ${lib}"
