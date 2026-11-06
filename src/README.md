# Internal implementation map

The installed public API remains under `include/regfw/`. Files here are private.

Optional async flow: `include/regfw/async.hpp` → `runtime/async.cpp` → existing MMIO engine under a short per-Bank state mutex. `runtime/access.cpp::prepare` admits declared deferred writes before behavior or transaction changes. Callbacks capture immutable tasks; workers run outside the state mutex and use `with_state` to update the Bank. Ordinary accesses do not enter the queue; there is no publication cache or route mirror. Synchronous endpoints use the same access semantics and execute captured work after unlocking.

| Folder | Responsibility |
|---|---|
| `dsl/` | Binding and validating declared access groups |
| `layout/` | Views, repeat expansion, runtime layout installation and the shared entity decoder |
| `runtime/` | MMIO access flow, transactions, callback binding, storage and lifecycle |
| `snapshot/` | Snapshot serialization and reset / restore state |
| `diagnostics/` | Logging, inspection, layout reporting and error formatting |
| `internal/` | Private state and decoder types shared between implementations |

**MMIO flow:** `runtime/access.cpp` → `internal/formula_decoder.hpp` → existing
execution and transaction functions in `runtime/access.cpp`.

**Registration flow:** `dsl/declarations.cpp` → `layout/*` → `runtime/lifecycle.cpp`
→ instance binding and prepared access metadata.

**Snapshot flow:** `snapshot/snapshot.cpp` and `snapshot/state.cpp`; do not change
schema or VMState contracts during structural refactors.

No header under `src/internal/` is installed. The public `include/regfw/` paths are
kept stable for existing device code.
