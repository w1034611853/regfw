# hlog 3.2.1


Fix C++ positional bit-fields passed to `HLOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL`, console macros, direct `Logger::write_unchecked` / `console_write`, and `QueryOutput::print`.


The old `Args&&` boundary attempts to bind a non-const reference to a bit-field and fails before fmt can format it. The synchronous formatting boundary now takes `const Args&...`: C++ can bind a temporary value for a bit-field, while ordinary objects, including noncopyable custom-formatted values, stay referenced. No arguments or references are retained after formatting.


```cpp
struct Register { unsigned queue_pkt_cnt : 19; };
Register value{321};
HLOG_DEBUG(logger, "queue_cnt={}", value.queue_pkt_cnt);
```


Compile-time fmt checks, hexadecimal/decimal formatting, argument evaluation once per enabled call, and disabled-log laziness are retained. No C ABI or SONAME change: ABI 3 and `libhlog.so.1` remain in use. The fix is entirely in the C++ header, so projects already using hlog 3.2.0 can replace `include/hlog.hpp` and rebuild their C++ callers. New distribution metadata is 3.2.1 for both runtime and Rust client.


`tests/cpp_format_args.cpp` includes the three reported POE scheduler log formats, unsigned/signed/bool/64-bit/const bit-fields, console/query output, noncopyable and tracked objects, and disabled logging. `bash tests/test_hexdump_headers.sh` builds and runs it with actual fmt under C++17 and C++20 and checks that invalid format strings still fail compilation.
