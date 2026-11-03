#!/usr/bin/env bash
# Format (or check) first-party C++ sources only. Third-party code is never rewritten.
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v clang-format-17 >/dev/null 2>&1; then
    fmt=clang-format-17
elif command -v clang-format >/dev/null 2>&1; then
    fmt=clang-format
else
    echo 'clang-format 17+ is required (e.g. sudo apt install clang-format-17)' >&2
    exit 2
fi
mapfile -d '' sources < <(find include src tests examples docs/snippets benchmarks \
    -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' -o -name '*.h' \) \
    -print0 | sort -z)
if [[ "${1:-}" == '--check' ]]; then
    "$fmt" --dry-run --Werror "${sources[@]}"
else
    "$fmt" -i "${sources[@]}"
fi
