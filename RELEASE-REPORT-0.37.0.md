# regfw 0.37.0 — Readability and module organization

## Scope

This release reorganizes and reformats **the existing 0.36.0 implementation**.
It is deliberately not an address-dispatch redesign. The public headers stay
at their original installed paths; REG/REG_LAYOUT/REG_REPEAT_ENTITY, MMIO,
PreAccess, Resolver, access group, callbacks, transactions, and snapshot APIs
are preserved. Snapshot format is unchanged.

### Main changes

- Consistent C++20 formatting (`.clang-format`), expanded compressed function bodies,
  `if`/`for` blocks, templates, lambdas, and long declarations. This naturally
  increases *physical* source lines, but not the amount of executable logic.
- Organized private code into `src/dsl`, `src/layout`, `src/runtime`,
  `src/snapshot`, `src/diagnostics`, `src/internal`.
- Split the old 2,759-line (after formatting) `runtime.cpp` into coherent
  implementations for access, lifecycle, bindings, handles, callbacks,
  behaviors, snapshot state, and diagnostic formatting.
- Split the old private `runtime_impl.hpp` into `entity_state.hpp`,
  `access_context.hpp`, and `formula_decoder.hpp` (with a small private
  forwarding umbrella).
- Kept `include/regfw/*.hpp` paths stable for downstream QEMU code.
- Updated CMake, internal test paths and documentation generation; made the
  generator tolerant of standard C++ whitespace.
- Moved historical delivery reports into `docs/history/`; added a Chinese
  architecture guide, a formatting helper and an opt-in private GitHub
  publication script.

## Verification

| Check | Result |
|---|---|
| Original 0.36.0, GCC 14.2 Release `-Wall -Wextra -Werror` | Pass |
| Original 0.36.0 CTest | **35/35** |
| 0.37.0, GCC 14.2 Release `-Wall -Wextra -Werror` | Pass |
| 0.37.0 CTest | **35/35** |
| 0.37.0, Clang 17 Release `-Wall -Wextra -Werror`, library | Pass |
| Compiled strong exported symbol set versus 0.36.0 | **224/224 unchanged** |
| `cmake --install` and independent `find_package(regfw 0.37.0 EXACT)` | Pass |
| C++ formatting pass and re-format stability | Pass on first-party sources |
| Documentation generation and link validation | Pass |
| Real POE/DAG QEMU integration and 10-module initialization | Not available in this environment |
| ASan/UBSan full suite | Not performed for 0.37.0 |

The strong-symbol comparison is a useful link-compatibility check, not a
blanket guarantee that every possible binary compatibility invariant is
unchanged. Rebuild QEMU with the matching 0.37.0 headers and static library.

## Compatibility / behavior notes

- Shared `EntityLayout` Hash and runtime registration remain 0.36.0's
  implementation, including its runtime installation-phase geometry checks.
- No new cache, lookup layer, `consteval` layout compiler, hook behavior or
  transaction path is introduced here.
- Large arrays still need their actual `unordered_map` entries during
  installation. This release does not claim an MMIO throughput improvement.
- Device-specific 4-minute layout compilation and machine init still require
  measurement on the real POE/DAG application.

## Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

For code navigation see [架构说明](docs/Architecture.zh-CN.md) and
[src/README.md](src/README.md). Use `scripts/format_cpp.sh` to format
first-party code with clang-format 17+ (or `--check` to verify it).

## GitHub publishing

A local Git repository with two commits and tag `v0.37.0` accompanies the
separately downloadable Git bundle. The repository has **not** been created
or pushed on GitHub: the current connected GitHub interface supports file/
commit operations on *existing* repositories but does not expose new-repo
creation. To publish a **private** new repo, clone the Git bundle or extract
the source package and run `scripts/publish_github.sh` from a system with
GitHub CLI (`gh auth login` required). Avoid making device source public by
accident.
