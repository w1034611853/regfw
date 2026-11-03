> 状态（2026-11-01）：已完成 StaticBuild/StaticWord/StaticPlan/installer/宏删除；
> `tests/retired/` 归档旧内部测试。动态布局合并与 MMIO 5% 目标仍未完成。
> 本文保留为当时的执行计划。

# regfw 0.35.0 legacy-backend removal plan

Status: correctness fixes and a frozen decode baseline are implemented; the
word-table backend is still present. This note records the exact remaining
deletion sequence so the next change is mechanical rather than exploratory.

## 1. Freeze the observable decode contract

Done before deletion:

- `tests/golden/battery_words.inc` is generated from the 0.34.0
  `StaticWord` plan for the mixed battery layout: 1321 words with
  address/target/binding/linear/byte/available/scope/identity/flow/route and
  effective policy.
- `tests/formula_golden.cpp` compiles only the formula plan and checks every
  golden word, including scope priority, identity family, route and flow.
- `tests/formula_geometry.cpp` covers absolute-coordinate overlap and a
  non-zero view base with a half-open reserve.

These two tests are the replacement for the compile-time word differential.
Keep them in the archive and in CI before removing the old builder.

## 2. Extract FormulaBuild

`formula_run` currently instantiates `StaticBuild<true>` only to walk
`StaticView`/`StaticMount`/`StaticTarget`/`StaticRepeatMount` and to record:

- `bindings` (`StaticBinding`) and the open-addressed binding index;
- `scopes` (`StaticScope`) and repeat/identity metadata;
- `identities` (`StaticIdentity`);
- `mounts` (`ElementMount` with `StaticElementPlan`);
- `status`, `root_type`, `emitted_nodes`, `first_binding`.

Create `FormulaBuild` in `include/regfw/static_layout.hpp` with those fields and
only these methods: `fit`, `scope`, the four `flatten` overloads, `mount`,
`emit_bundle`, `emit_register` (measuring branch only), `scope_identities`.
Delete `candidates`, `words`, `shadows`, `redirects`, `pages`, `page_keys`,
`candidate_count`, `FastStats`, `finish`, `count_regular`, `stream_disjoint`,
`compare` and `mount_spans`.

Switch `formula_run` to `FormulaBuild b;` and delete the `measuring` flag.
Run the full suite; behavior must be identical.

## 3. Delete the old production path

- `include/regfw/detail/static_plan.hpp`: delete `StaticWord`, `StaticShadow`,
  `StaticPlanView`, `StaticPlanView::add_page` and the full-plan-only fields.
  **Keep** `StaticBinding`, `StaticScope`, `StaticIdentity`, `ElementRecord`,
  `STATIC_NONE`, `StaticFlow`, `StaticLookupPage`, `StaticLookupSlot`,
  `static_word_hash`, `static_page_hash`: the dynamic layout's `SparseLookup`
  still uses the lookup types.
- `include/regfw/static_layout.hpp`: delete `StaticPlan`, `compile_static_layout`,
  `install_static_layout`, both `REGFW_LEGACY_STATIC_WORDS` branches and the
  old includes they alone need.
- `src/static_layout.cpp`: delete the file and remove it from `CMakeLists.txt`
  and `build.mk` `LIB_SRCS`.
- `include/regfw/types.hpp`: remove `friend struct StaticInstaller`.
- `include/regfw/runtime.hpp`: remove `friend struct StaticInstaller`; keep
  `compiled_views_`/`compiled_access_`/`compiled_view_mode_` because the
  dynamic layout still uses them.
- `src/runtime.cpp`: remove only the legacy static install entry points and the
  `compiled_redirects_` expansion used by the word plan; keep the dynamic
  `compiled_views_` path and the formula path. `finalize` keeps its
  `formula_` guard.

## 4. Migrate tests and benchmarks

The old tests assert internals that no longer exist. Replace, do not weaken:

| file | action |
|---|---|
| `tests/formula_diff.cpp` | remove `compile_static_layout`, `Battery<1>` word pass and runtime mode 1; keep dynamic-vs-formula runtime differential; add golden check |
| `tests/static_regular.cpp` | replace word/page/redirect assertions with `formula_run` mount/array assertions + golden decode; keep layout names/priorities/identities |
| `tests/static_builder.cpp` | fold into formula shape/spans test; delete the builder-specific checks |
| `tests/static_layout.cpp` | drive `compile_formula_layout` + `install_static_layout` replacement (`install_formula_layout`); keep reserve/RO/alias cases |
| `tests/symbolic_layout.cpp` | drop `REGFW_LEGACY_STATIC_WORDS`; assert formula plan + installed behavior |
| `benchmarks/plan_digest.cpp` | digest the formula plan (mounts/arrays/scopes), not words |
| `benchmarks/poe_layout.cpp` | delete `POE_FORMULA` legacy branch and `POE_FORCE_FULL` word mode |
| `benchmarks/measure_formula.py` | delete legacy cases; keep formula compile/RSS/init/MMIO |

Keep `formula_geometry` and `formula_golden` as the two non-negotiable gates.

## 5. Acceptance after deletion

1. `rg 'StaticWord|StaticShadow|StaticPlanView|compile_static_layout|REGFW_LEGACY_STATIC_WORDS|StaticInstaller' include src` returns nothing.
2. Strict clang build `-Wall -Wextra -Werror`; full local suite.
3. `formula_golden` still reports 1321 words.
4. Rebuild POE/DAG/scale compile-time and RSS; confirm formula numbers do not
   regress (they should improve by the removed instantiations).
5. Re-run MMIO benchmarks; the remaining 1.5-1.9x gap is independent of this
   deletion and must be fixed in `FormulaDecoder::fast_access`/identity path.
