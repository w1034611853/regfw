#!/usr/bin/env bash
# No Rust required. C ABI mock + QEMU stubs, not a real QEMU/runtime test.
set -euo pipefail
test_root="$(cd "$(dirname "$0")/.." && pwd)"
test_build="$(mktemp -d)"
echo "Test build directory: $test_build"
test_stubs="$test_root/tests/qemu-stubs"
cc_flags=(-std=gnu11 -Wall -Wextra -Werror)
gcc "${cc_flags[@]}" -I"$test_root/include" "$test_root/tests/c_console_macros.c" -o "$test_build/test-c"
"$test_build/test-c"
gcc "${cc_flags[@]}" -fPIC -shared "$test_stubs/src/fake-hlog-runtime.c" -o "$test_build/fake.so"
gcc "${cc_flags[@]}" -DHLOG_TEST_OLD_RUNTIME -fPIC -shared "$test_stubs/src/fake-hlog-runtime.c" -o "$test_build/fake-old.so"
for test_name in test-hlog-loader test-hmp-console; do
    gcc "${cc_flags[@]}" -I"$test_stubs/include" -I"$test_root" \
        "$test_root/qemu/hlog-loader.c" "$test_stubs/src/qemu-stubs.c" \
        "$test_stubs/src/$test_name.c" -ldl -o "$test_build/$test_name"
done
"$test_build/test-hlog-loader" "$test_build/fake.so" "$test_stubs/hlog-readconfig.conf"
"$test_build/test-hmp-console" "$test_build/fake.so" new
"$test_build/test-hmp-console" "$test_build/fake-old.so" old
echo "All C/header and mock QEMU control tests passed; real Rust tests are separate."
