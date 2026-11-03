# regfw 0.35.2 – Prepared EntityLayout AccessInfo Hash (evaluation build)

## Architecture

The **default in this evaluation package** is `REGFW_PREPARED_ACCESS_HASH=ON`:

    MMIO address → containing View range(s), respecting priority and holes
                 → one shared, immutable EntityLayout::unordered_map lookup
                 → PreparedAccessInfo + per-View PreparedMountContext
                 → existing Scalar/Mapped/Generic access executor

This is a prototype of a replacement decoder, not a claim that regfw's existing
Access Engine has been rewritten, nor that all legacy code has been removed.
The older decoder remains available only for a controlled A/B build with
`-DREGFW_PREPARED_ACCESS_HASH=OFF`.

### What `.second` contains

Each local 4-byte address maps to `PreparedAccessInfo`:

- REG/mount ordinal (within an immutable shared geometry layout)
- array element's linear index
- byte offset within the register
- remaining bytes within the element
- an already computed `RegElementIndex` with **rank and coordinates array**

`RegElementIndex` supports ranks 1–8, including sparse/strided arrays; there
are no fixed `index0` and `index1` fields and no per-access dynamic allocation.
The index is copied from the existing Entity layout **once during initialization**.
There is no per-access re-inversion of dimensions/strides.

The `PreparedMountContext` is bound once per mounted View. It contains the
per-instance EntityState, its stable layout, route, permission policy, source
instance, identity reference, View PreAccess chain and diagnostic names.
This separation is essential: VF aliases share **immutable geometry**, not
Storage, snapshot/transaction data or replaceable callback function values.
Hooks and Snapshot data continue to be reached through stable EntityState
slots and the existing execution engine. Runtime resolver outputs cannot be
precomputed.

The prepared path does not build a runtime Zone/Candidate/Scalar Run/Page
Directory. For diagnostics, mapped words can still be enumerated on demand.
Actual occupied bytes (rather than entire View envelopes) are compared at
layout installation to detect collisions, handle transparent holes and issue
shadow warnings. Cross-word wide-register checks remain conservative.

## Compatibility and limitations

- Public REG/REG_LAYOUT/REG_BUNDLE DSL, MMIO API, callback signatures, reset and
  snapshot format are unchanged.
- The previous dynamic and static layout APIs both use this backend.
- The CMake definition is propagated to tests that include private runtime
  headers, avoiding incompatible class layouts across translation units.
- An individual RegisterMap still requires serialized access, as in the old
  implementation; `DispatchGuard` is not a multithreaded lock.
- This code branch still contains the previous decoder behind the A/B option.
  It must not be described as a fully deleted legacy backend.
- The full user POE/DAG/QEMU integration is unavailable in this working tree;
  synthetic layouts and framework tests cannot substitute for it.
- Performance must be judged from same-machine repeated A/B tests; preliminary
  runs show no universal improvement for `std::unordered_map` over formula
  decoding. Do not promote this evaluation build without real POE measurements.

## Build and run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREGFW_PREPARED_ACCESS_HASH=ON -DREGFW_BUILD_TESTS=ON \
  -DREGFW_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/bench_mmio_dynamic 2 4 0 0 random 0
```

Build with `REGFW_PREPARED_ACCESS_HASH=OFF` for a same-source legacy A/B
measurement. Do **not** enable both decoder designs simultaneously.

## Evidence and next decision

The baseline from the previous prototype was a Hash lookup followed by the
old FormulaDecoder reassembly. This revision removes the Zone/Page lookup from
the prepared runtime path, stores index coordinates in `.second`, and binds
per-View access context at installation.

A synthetic GCC 14.2 Release A/B comparing the *previous Hash prototype* to
this prepared prototype (one run, medians in ns/access; lower is better):

| Workload | Previous Hash | Prepared Hash |
|--|--:|--:|
| Scalar hot read | 26.75 | 26.23 |
| Scalar random read | 47.99 | 50.14 |
| VF mapped read | 169.45 | 151.55 |
| Callback hot read | 44.90 | 46.38 |
| Stride-64 mixed random read | 81.99 | 89.84 |

These values have measurement noise and are **not evidence of a universal
speedup**. The next step, if business integration validates semantics, is to
profile *View selection*, *Hash lookup*, and *access executor* separately,
then decide whether to retain this simpler backend. Do not add mutable caches.

## Validation

GCC 14.2 Release `-Wall -Wextra -Werror` core build and CTest are used for
this evaluation. The test suite also includes a new four-dimensional
`RegElementIndex` callback test with hook registration after finalize. The actual
final build/test results are recorded with this package's delivery manifest.
