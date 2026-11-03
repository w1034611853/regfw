# Internal implementation map

The installed public API remains under `include/regfw/`. Files here are private.

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
