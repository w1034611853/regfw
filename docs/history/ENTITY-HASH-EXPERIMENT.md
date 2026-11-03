> HISTORICAL PROTOTYPE A REPORT. This is not the current prepared AccessInfo implementation.
> For the latest code and build commands, see `PREPARED-ACCESS-REPORT.md`.

# Shared EntityLayout + std::unordered_map: Prototype A

This is an **experimental A/B branch**, not a stable regfw release. Its base is
regfw 0.35.2 Cleanup Pass 3 (cacheless). The public REG DSL, MMIO APIs and
execution engine are unchanged.

## Enable / disable

```bash
cmake -S . -B build-hash -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DREGFW_EXPERIMENT_ENTITY_HASH=ON -DREGFW_BUILD_TESTS=ON \
  -DREGFW_BUILD_BENCHMARKS=ON
cmake --build build-hash --target regfw_formula_diff_test \
  regfw_formula_regressions_test regfw_address_view_test bench_mmio_dynamic -j2
ctest --test-dir build-hash -R 'formula_diff|formula_regressions|address_view' \
  --output-on-failure
REGFW_ENTITY_HASH_STATS=1 ./build-hash/bench_mmio_dynamic 0 4 1 0 random 0
```

With `REGFW_EXPERIMENT_ENTITY_HASH=OFF`, the unchanged Pass 3 decoder runs.
The experiment is explicitly opt-in, not a behavior change for default users.

## Implementation

- Group a REG mount under a suitable enclosing View. Small one-REG repeat
  leaves are grouped into their parent View to avoid a thousand singleton maps.
- Build `EntityHashLayout::words` **at runtime** using native C++, no consteval
  hash tables. Each local 4-byte key maps to `(mount ordinal, linear, byte)`.
  The existing `RegElementIndex` metadata for index0/index1 is already
  available from the referenced EntityState; no per-access formula recalculation.
- Intern geometrically identical layouts, independent of storage and VF identity.
  Each View has only an ordered vector of its own mount/binding references.
- For View selection, binary-search sorted View starts and scan potentially
  overlapping ranges; compare the original scope priorities after a true
  occupied-address Hash hit. Preserve reserve, permission, callback, identity
  and wide-register semantics through the unchanged Access Engine.
- The old Zone/Run/Page data are **retained for diagnostic/differential
  checks in this prototype**. Hash only replaces the hot `find_mount` selection
  when the build option is enabled; this is NOT a completed code reduction.
- A grouped View with internal priority aliases resolves the winner per key
  at initialization and does not share that ambiguous table across groups.

## Independent memory measurement (Linux GCC 14.2 / libstdc++)

`benchmarks/entityhash_memory.cpp` creates one native unordered_map and lets
8 alias Views share one `shared_ptr<const Layout>`. `mallinfo2().uordblks`
measures the approximate *incremental live heap allocation*; not total RSS.

| Elements | Bytes/element | Word keys | Additional heap |
|---:|---:|---:|---:|
| 10,000 | 4 | 10,000 | 562,416 B (~0.54 MiB) |
| 10,000 | 64 | 160,000 | 7,680,224 B (~7.32 MiB) |
| 10,000 | 256 | 640,000 | 30,720,224 B (~29.30 MiB) |

Compile this standalone memory benchmark using GCC -O2: 0.62 seconds,
peak RSS ~104,828 KiB; the native map is built at **execution time**.
This is not a claim that building 10,000 distinct REG C++ declarations has the
same compiler cost.

For the framework's mixed 4-byte benchmark:

```
REGFW_ENTITY_HASH_STATS=1 ./build-hash/bench_mmio_dynamic 0 4 1 0 random 0
# views=18, unique_layouts=2, total_view_keys=6200,
# estimated additional Hash allocation ~44,176 bytes
```

## Preliminary MMIO A/B (same machine, GCC 14.2 Release)

Existing `benchmarks/mmio.cpp`, 100k-address trace, repeated. Approximate
median ns/operation below; this is one measurement run, not a formal confidence
interval. The baseline is unmodified cacheless Pass 3.

| Workload | Pass 3 | Prototype A | Finding |
|---|---:|---:|---|
| 4B scalar hot read | 21.6 | 25.7 | slower |
| 4B scalar random read | 31.5 | 40.5 | slower |
| VF mapped read | 121.2 | 166.4 | slower |
| Callback hot read | 58.7 | 84.2 | slower |
| 64B-stride mixed random read | 65.1 | 84.8 | slower |

Eight extra read/write/reference-switch validation combinations (two strides,
with/without VF mapping, with/without callbacks) pass. The following targeted
CTest tests were built and passed with Hash enabled:

- address_view, formula_diff, formula_regressions;
- static_layout, formula_geometry, formula_golden, formula_fastpaths, snapshot;
- symbolic_layout, index_mapping, view_pre_access, read_callbacks,
  mapped_logging, fast_path.

**Total targeted CTest: 14/14.** A complete full build/CTest run was attempted
but was interrupted by the execution time limit while compiling additional
historical test targets; full-suite status is **not claimed**. No real POE/DAG
QEMU build or sanitizer run has been completed for this experiment.

## Conclusions / restrictions

1. 10k static metadata entries do **not** imply hundreds of MB, nor a
   consteval/compiler OOM. Runtime hashes are modest; wide elements multiply
   the key count only if decoding is keyed per 4-byte word.
2. `std::unordered_map` is functionally viable, but the measured whole-framework
   dispatch is slower than existing Pass 3 on these synthetic traces.
3. To replace production, test a true large-Entity POE/DAG workload, rather than
   assuming a simple `unordered_map` lookup makes all MMIO faster.
4. Do not remove the original backend or select this as the default until
   performance, memory and the full suite meet the required thresholds.
