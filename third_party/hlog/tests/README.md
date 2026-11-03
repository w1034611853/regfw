# HLog verification (3.2.1)

The actual runtime was built and installed with Cargo/rustc 1.75.0. C/C++ checks used GCC 13.3, C11/C++17/C++20 and real fmt 9.1 headers.

## Bit-field regression (3.2.1)

`cpp_format_args.cpp` uses real fmt and a captured C ABI to check the three reported POE scheduler log patterns, unsigned/signed/bool/64-bit bit-fields, const bit-fields, file and console logs, direct Logger APIs, query output, noncopyable/custom formatted objects, no extra copies, exact-once argument evaluation and disabled-log laziness. `test_hexdump_headers.sh` runs it in C++17 and C++20 and rejects invalid format strings in both modes.

## Executed for this change

- Runtime test binaries: 10 tests passed, including byte/native-word formatting, fixed ASCII columns, partial groups, empty input and invalid formats, plus existing query/config/rotation-family tests.
- Rust client: compiled against the installed runtime; the new hexdump macro integration test and hexdump example passed.
- `bash tests/test_hexdump_headers.sh`: C and C++17/C++20 boundary tests passed for C arrays/array/vector/span, typed element byte counts, default/native logger handles, filtering, once-only evaluation and lazy arguments. Pointers without a length and nontrivial elements were rejected. Only the logger ABI is mocked; fmt is real.
- `python3 tests/test_hexdump_runtime.py lib/libhlog.so.1`: passed against the **real installed runtime** for all four formats at lengths 0..35 (144 combinations), unaligned input, legacy byte API, complete prefixes, native words, `??` tails, ASCII alignment, file/console equality, console-only output while OFF, invalid inputs and concurrent three-row batches.
- `python3 tests/test_console_runtime.py lib/libhlog.so.1`: existing real-runtime routing/color/rename/mirroring/concurrency tests passed.
- C++ hexdump and console examples compiled and ran with actual fmt and hlog. Actual output is recorded in `examples/HEXDUMP_PREVIEW.txt`.
- `bash tests/test_headers_loader.sh`: existing C console wrappers, QEMU loader/readconfig/HMP wrappers, old-runtime capability probing and 18 query filter cases passed against **QEMU stubs and a fake control runtime**.
- `bash scripts/verify_runtime.sh lib/libhlog.so.1`: SONAME, ABI v3 and new formatted hexdump exports passed.

This is not a full QEMU build or a real device-model integration test. Those checks remain to be run in the consuming project.

## Reproduce

From the hlog source root, with real Cargo, GCC/G++, Python 3, gzip and fmt:

```bash
bash scripts/install.sh
bash scripts/verify_runtime.sh ./lib/libhlog.so.1
LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" cargo test --workspace
bash tests/test_hexdump_headers.sh
python3 tests/test_hexdump_runtime.py ./lib/libhlog.so.1
python3 tests/test_console_runtime.py ./lib/libhlog.so.1
bash tests/test_headers_loader.sh
```

The real-runtime Python tests isolate each child in a temporary working directory because loading the library initializes the default log file. To test the project's bundled fmt instead of system fmt, set `HLOG_TEST_SPDLOG_INCLUDE=/absolute/path/to/spdlog/include` for the header test.

Compile `examples/cpp_hexdump.cpp` with C++20 (or C++17 without the span portion), `-DFMT_HEADER_ONLY=1`, the hlog/fmt include directories, `-lhlog` and an rpath to the installed hlog library. Run the Rust example with `LD_LIBRARY_PATH` set to the hlog lib directory and `cargo run -p hlog --example hexdump`.

Finally, build the real QEMU tree, exercise HMP `hlog console on/off/show`, and dump a device buffer through the new macros. Logs use host stderr, not a guest serial socket. File logs/archives must not contain console-only records or ANSI color escapes.
