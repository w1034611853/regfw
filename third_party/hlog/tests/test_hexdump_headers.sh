#!/usr/bin/env bash
# Real fmt headers; mock only the hlog ABI to inspect forwarding/laziness.
set -euo pipefail
test_root="$(cd "$(dirname "$0")/.." && pwd)"
test_build="$(mktemp -d)"
trap 'rm -rf "$test_build"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -I"$test_root/include" \
    "$test_root/tests/c_hexdump_macros.c" -o "$test_build/c-test"
"$test_build/c-test"
cpp_flags=(-Wall -Wextra -Werror -DFMT_HEADER_ONLY=1 -I"$test_root/include")
# Optionally use the project's bundled fmt instead of system <fmt/format.h>.
if [[ -n "${HLOG_TEST_SPDLOG_INCLUDE:-}" ]]; then
    cpp_flags+=(-DHLOG_USE_SPDLOG_FMT=1 -I"$HLOG_TEST_SPDLOG_INCLUDE")
fi
for cpp_std in c++17 c++20; do
    g++ -std="$cpp_std" "${cpp_flags[@]}" "$test_root/tests/cpp_hexdump_macros.cpp" -o "$test_build/cpp-test"
    "$test_build/cpp-test"
    g++ -std="$cpp_std" "${cpp_flags[@]}" "$test_root/tests/cpp_format_args.cpp" -o "$test_build/format-args-test"
    "$test_build/format-args-test"
done
printf '#include "hlog.hpp"\nint main(){auto logger=hlog::Logger::from_handle(nullptr);HLOG_DEBUG(logger,"{:d}","text");}\n' > "$test_build/invalid-format.cpp"
for cpp_std in c++17 c++20; do
    if g++ -std="$cpp_std" "${cpp_flags[@]}" -fsyntax-only "$test_build/invalid-format.cpp" >"$test_build/format-errors" 2>&1; then
        echo "ERROR: invalid format was accepted under $cpp_std" >&2
        exit 1
    fi
done
for invalid in pointer nontrivial; do
    if [[ "$invalid" == pointer ]]; then
        declaration='unsigned char storage[4]{}; auto buffer=storage;'
    else
        declaration='std::vector<std::string> buffer{"uninitialised representation"};'
    fi
    {
        printf '#include "hlog.hpp"\nint main(){auto logger=hlog::Logger::from_handle(nullptr);'
        printf '%s' "$declaration"
        printf 'HLOG_HEXDUMP_DEBUG(logger,hlog::u32,buffer);}\n'
    } > "$test_build/invalid.cpp"
    if g++ -std=c++20 "${cpp_flags[@]}" -fsyntax-only "$test_build/invalid.cpp" >"$test_build/errors" 2>&1; then
        echo "ERROR: unsafe $invalid buffer was accepted" >&2
        exit 1
    fi
done
echo "C/C++ logger and hexdump headers passed, including bit-fields and expected invalid-format/buffer rejections"
