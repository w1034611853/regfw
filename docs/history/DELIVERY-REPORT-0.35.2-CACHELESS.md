# regfw 0.35.2 — Cleanup Pass 3: cacheless Formula Decoder

## Scope and design decision

This pass builds on **cleanup-pass2**, not the unmodified 0.35.2 archive. It
removes the two **mutable, per-address hot-path caches**, without changing any
user-facing REG/REG_LAYOUT, MMIO, resolver, callback, transaction, snapshot or
CMake API:

- `FormulaDecoder::word_cache_tags_`, `word_cache_slots_`, and their 4-way
  hit/probe/replacement logic are **deleted**, not merely disabled.
- `FormulaDecoder::zone_cache_`, its tag/hash/probe/write, and the zone-cache
  helper are **deleted**.
- `locate_zone()` performs a read-only binary search. `fast_access()` derives
  a temporary access plan directly from immutable pattern/binding metadata.
- **Scalar Run and Sparse Page Directory are retained**. They are immutable
  dispatch indexes built at installation, not mutable caches. A controlled
  variant deleting both was built and benchmarked; on the measured random
  scalar workload, latency rose >4x vs baseline. It would be misleading to
  present that variant as a performance improvement.
- The separate manual (non-View) `aliases_` precomputed mapping remains
  because it serves supported public manual mapping APIs and is not part of
  the Formula Decoder hot-path caches. Removing it would require a distinct
  API/semantic audit.

FormulaDecoder no longer performs per-address cache writes from `const` find
methods. This reduces shared mutable state; it **does not imply that the whole
RegisterMap is thread safe**. `DispatchGuard`, storage and callbacks still
need the project's documented external access serialization.

### Size

| Metric | Cleanup Pass 2 | Cleanup Pass 3 |
|---|---:|---:|
| regfw core physical lines (`include/`, `src/`) | 10,059 | 9,980 |
| `src/detail/runtime_impl.hpp` | 987 | 908 |
| Fixed mutable Formula Decoder cache data | ~144 KiB/instance | 0 |

144 KiB is based on 1024 zone slots x 16 B plus 4096 word slots x 24 B
and 4096 word tags x 8 B on the tested x86-64 ABI.

## Verification

- GCC 14.2 Release with `-Wall -Wextra -Werror`: all library and CTest
  binaries built successfully.
- **34/34 CTest passed**; includes formula geometry/diff/fastpaths,
  dynamic-state callback changes, overlapping scopes, identity, snapshot,
  static layouts, separate-TU and compile-fail contracts.
- Clang 17 Release `-Wall -Wextra -Werror`: core, `formula_fastpaths`,
  `formula_regressions` built and both tests passed.
- Exported strong (`T`) symbol name set remained **224/224 identical**.
- No real POE/DAG or QEMU machine benchmark was available.

## Paired dynamic-MMIO performance study

Bench: existing `benchmarks/mmio.cpp` with 100,000-address traces,
`-O3`, GCC 14.2, one pinned CPU (`taskset`), `REGFW_BENCH_THREADS=1`.
Each variant was executed in alternating order across three blocks; each
measurement uses the C++ harness's eight rounds and median of its last seven
rounds; table is the median of three process measurements. The benchmark
also checks full read/write/storage/callback equivalence against its hand-
written switch reference and equality of output checksums across variants.
Small differences are measurement-sensitive; this is **a synthetic microbenchmark**,
not a QEMU end-to-end performance claim.

- Baseline: cleanup-pass2 (both caches + immutable indexes).
- Cacheless: this delivery (no mutable caches, retains immutable indexes).
- Minimal experiment: caches **and** Scalar Run and Page Directory removed,
  retaining only Zone binary search (not included as delivery source).

| Workload (stride / shape / callback / pattern / operation) | Baseline ns | Cacheless ns | Minimal ns | Cacheless change |
|---|---:|---:|---:|---:|
| 4 / plain / rw / hot / read | 13.365 | 22.172 | 29.428 | +65.9% |
| 4 / plain / rw / hot / write | 19.575 | 27.501 | 31.470 | +40.5% |
| 4 / plain / rw / random / read | 20.900 | 32.895 | 92.460 | +57.4% |
| 4 / plain / rw / random / write | 27.133 | 39.225 | 96.047 | +44.6% |
| 4 / mixed / rw / hot / read | 43.903 | 38.056 | 45.540 | -13.3% |
| 4 / mixed / rw / random / read | 68.482 | 75.203 | 125.006 | +9.8% |
| 4 / plain / callbacks / mapped / read | 128.052 | 120.426 | 143.184 | -6.0% |
| 4 / plain / callbacks / mapped / write | 122.876 | 117.283 | 143.559 | -4.6% |
| 4 / plain / callbacks / random / read | 50.359 | 63.996 | 101.052 | +27.1% |
| 4 / mixed / callbacks / mapped / read | 124.599 | 124.329 | 154.759 | -0.2% |
| 4 / mixed / callbacks / hot / read | 66.484 | 66.196 | 74.940 | -0.4% |
| 64 / plain / callbacks / mapped / read | 124.954 | 124.420 | 148.828 | -0.4% |

### Decision

The **cacheless** variant is delivered because the user explicitly prioritizes
simple, immutable dispatch. It has a *real and material* Scalar32 regression:
plain hot read ~13.4 -> 22.2 ns and plain random read ~20.9 -> 32.9 ns.
Callback and mapped cases are broadly similar, with some improving.
Do **not** classify this pass as a throughput optimization or deploy it as a
performance-neutral upgrade without real POE profile confirmation.

The minimal no-index variant is not shipped: removing essential readonly
indexes increases plain random read to ~92.5 ns and has no functional benefit
apart from lower code size. These indexes support efficient formula lookup
without storing a 4-byte per-address table.

## Next logical simplification

Do not reintroduce Word Memo/Zone Cache to mask slow arithmetic search.
Profile the actual POE MMIO trace, then reduce the number of branch/search
stages and duplicated temporary contexts in formula decode; keep the one
shared access/transaction/callback engine. Evaluate alternative read-only
range or pattern dispatch, but retain correctness and A/B evidence.
