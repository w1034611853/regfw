# regfw 0.35.2: core line-count audit and safe deduplication (pass 1)

## Counting convention

Counts include all physical lines (comments and blank lines), use UTF-8 `splitlines()`, exclude binary `.so` files and generated build files. “Core” means `include/` and `src/` C/C++ files only. Third-party hlog is *not* the regfw core. `extras/vmstate_fields.hpp` is counted separately.

| Area | Baseline 0.35.2 | After pass 1 |
|---|---:|---:|
| `include/` | 5,014 | 5,020 |
| `src/` | 5,116 | 5,093 |
| Core total | **10,130** | **10,113** |
| Core nonblank | 9,910 | recompute as needed |
| `extras/vmstate_fields.hpp` | 539 | 539 |

The line reduction is intentionally small: a line-count target must never justify removing supported public functionality or destabilizing MMIO hot paths.

## Core change

- `include/regfw/detail/formula_plan.hpp`: added one shared `constexpr classify_interleaved` routine, returning explicit success/fallback/error categories.
- `include/regfw/static_layout.hpp`: static compiled-layout path calls that routine while preserving its existing exception messages and Table fallback.
- `src/address_view.cpp`: dynamic AddressView path calls the same classifier while preserving its existing error reporting and Table fallback.
- No changes to public DSL, storage, resolvers, callbacks, transactions, snapshot format, runtime decode hot path, or CMake API.

This removes two independent implementations of multi-dimensional stride ladder feasibility, GCD/self-overlap checks and modular inverse setup. It is architectural de-duplication, not code minification.

## Validation

- Baseline: GCC 14.2, Release, `-Wall -Wextra -Werror`, **34/34 CTest**.
- Pass 1: same toolchain/options, **34/34 CTest**.
- No real POE/QEMU machine integration test was available. The user should still run their existing integration tests before deploying.

## Areas requiring separate audited refactors

1. `src/runtime.cpp` (1,551 lines), `src/detail/runtime_impl.hpp` (987): mixed address resolution, Identity, transactions and fast-path handling. Refactor only with MMIO trace/performance baselines.
2. `src/layout_report.cpp` (793): diagnostic/report generation. Keep all report formats stable.
3. `include/regfw/static_layout.hpp`: trim duplicated metadata construction without increasing constexpr compile memory.
4. Vendored `third_party/hlog/` (13,392 text lines): dependency packaging is a separate task; do not count deleting it as regfw core LOC savings.
5. `tests/retired/` and `benchmarks/retired/` (607 lines combined): unbuilt pre-formula test artifacts; consider removing from a future distribution only if archived separately.

Do **not** delete `SparseLookup` simply because it resembles the removed StaticWord backend: it still supports manual register-map APIs.

---

# Pass 2 — Formula-only internal cleanup

This pass is based on the *pass-1* source tree, not on a previous historical
StaticWord backend. It preserves the public DSL, QEMU-facing MMIO API,
transaction semantics and snapshot format. The internal release version remains
`0.35.2`; the package suffix `cleanup-pass2` distinguishes this delivery.

## Changes and rationale

1. **One byte-interval overlap traversal** (`src/formula.cpp`): dynamic view
   validation, hidden same-priority collisions and compiled-static validation
   now use a shared absolute-address sweep. Binding, element index and actual
   byte-lane equivalence stay explicit. No MMIO hot-path decoder is changed.
2. **One occupied-byte Reserve traversal** (`src/formula.cpp`): both dynamic
   and static installers use the same sparse-element intersection predicate,
   while preserving their existing error and warning policies.
3. **Removed unused 0.33.x element-stream internals**
   (`include/regfw/static_layout.hpp`): deleted `static_dense_order`,
   `static_element_at`, `ElementCursor`, `MountSpan`, unused per-plan fields
   and unused `StaticTargetTraits` declaration. Actual FormulaPlan generation
   and the required `static_sparse_order` remain unchanged.
4. **Additional regression coverage** (`tests/formula_regressions.cpp`): a
   512-bit register partially overlapping a 32-bit register in its last lane
   must fail; placing the narrow register immediately after the 512-bit value
   must succeed.
5. **Retired, unbuilt old-backend tests**: the five files previously under
   `tests/retired/` and `benchmarks/retired/` (607 physical lines) are preserved
   in an optional *separate* `regfw-0.35.2-retired-tests.tar.gz` archive and
   omitted from the active SDK. None is referenced by CMake or CTest. The
   prior pass-1 package also contains them unmodified.

## Line count (physical C/C++ lines, no build files)

| Area | Original 0.35.2 | Pass 1 | Pass 2 |
|---|---:|---:|---:|
| `include/` | 5,014 | 5,020 | **4,975** |
| `src/` | 5,116 | 5,093 | **5,084** |
| **Core** | **10,130** | **10,113** | **10,059** |

Core decreases by 54 lines in pass 2, or 71 lines cumulatively, despite
retaining comments and readable code. The new regression test adds 38 lines to
the active test suite, which is not counted as core reduction. The additional
607 retired lines are a **distribution cleanup**, not claimed as core LOC.

## Independent verification

- GCC 14.2, Release, `-Wall -Wextra -Werror`: full clean build passed.
- `ctest --test-dir build -j2 --output-on-failure`: **34/34** passed.
- Clang 17, Release, `-Wall -Wextra -Werror`: core library and
  `regfw_formula_regressions_test` built and test passed.
- Exported strong global symbol names (`nm`, ELF `T`): **224 before and 224
  after; exact name set equal**. This does not replace a full ABI audit.
- Representative `benchmarks/compile.cpp` (`ELEMENTS=64`, GCC 14.2 `-O2`),
  **single run**: pass 1 3.56s / 325080 KiB peak RSS; pass 2 3.32s /
  324972 KiB. Differences are small and may be measurement noise; no general
  compile or MMIO speedup is claimed.
- No POE/DAG production tree, QEMU machine init, or full migration integration
  was available, so this release **must not** be represented as those
  validations being complete.

## Deliberately not changed

- No public `REG` / `REG_LAYOUT` / callback / resolver / transaction APIs.
- No `RegisterMap::try_mmio_*` or `FormulaDecoder::fast_access` hot-path edits.
- No snapshot schema, storage layout or reset behavior changes.
- No deletion of `SparseLookup`: it still serves manual register-map APIs.
- No flattening of code to artificially lower line counts.

## Suggested next controlled pass

Profile `runtime.cpp` and `runtime_impl.hpp` with a real POE address trace
before changing shared dispatch/transaction paths. Only then eliminate
measurable repeated lookups or state construction with trace and performance
regressions in place. Avoid merging the access and address-decode layers or
modifying public DSL just to shrink source files.

## Packaging and installed-consumer verification

- The pass-2 archive was independently extracted and all **210 packaged
  file hashes** verified against its regenerated `SHA256SUMS`.
- The extracted source, with `REGFW_BUILD_TESTS=OFF` and `-Wall -Wextra
  -Werror`, completed a standalone release library build and CMake install.
- A separate consumer built against that installation using
  `find_package(regfw 0.35.2 EXACT REQUIRED CONFIG)`, linked with
  `regfw::regfw`, and ran the public quickstart sample (`CONTROL=42`,
  `STATUS=7`, ignored read-only write). The vendored hlog path was supplied
  explicitly to the consumer's configure step.

---

## Pass 3 — mutable Formula hot-path caches removed

Word Memo (4-way x 1024 sets) and Zone Cache (1024 slots) were physically removed. Formula lookup is now readonly. Retained compact readonly Scalar Run and Page Directory because removing them measured up to ~4.4x slowdown on scalar random read. **Cacheless Scalar32 read regression is material**; see `DELIVERY-REPORT-0.35.2-CACHELESS.md` and `benchmarks/results-cacheless/paired-mmio-2026-11-02.json`. GCC 14.2 34/34 CTest; Clang 17 focused tests; public API unchanged.
