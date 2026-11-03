# regfw 0.35.0 DS 交付报告

日期：2026-11-01
工作树：`work/regfw-0.34.0-ds`（基于 0.34.0 DS 公式后端）
目标：按 m00986 删除旧逐 word 后端、统一静态解码路径、修 reserve/overlap/UB、补测试与性能报告。

## 1. 结论

已完成 **旧后端删除** 和 **静态/动态统一到 FormulaDecoder**：
`StaticBuild`/`StaticWord`/`StaticPlan`/`compile_static_layout`/
`install_static_layout`/`src/static_layout.cpp`/`REGFW_LEGACY_STATIC_WORDS` 全部移除；
静态布局只生成 `FormulaPlan`，动态 `AddressView` 由 `ViewCompiler` 安装为逐元素
`FormulaDecoder` Scalar mount；两条路径运行期都只走 `FormulaDecoder`，不再构造逐
word `compiled_views_` 表。

未完成：**MMIO 5% 性能目标未达到**（动态/静态公式仍约为 0.34 动态快路径的 2.1–2.2x），
**QEMU 实机/真实 POE/DAG 未验证**。CMake/CTest 32/32、ASan/UBSan、严格 clang，以及
GCC 16.2 strict 构建与 CTest 32/32 已在本机通过。因此本报告不宣称 0.35.0 全部完成。

## 2. 功能变更清单

### 2.1 删除

- `include/regfw/static_layout.hpp`：`StaticBuild<CountOnly>`、`StaticPlan`、
  `compile_static_layout`、`install_static_layout`、`REGFW_LEGACY_STATIC_WORDS` 分支。
- `include/regfw/detail/static_plan.hpp`：`StaticWord`、`StaticShadow`、`StaticPlanView`、
  `StaticInstaller` 前向声明。
- `src/static_layout.cpp`（逐 word installer）。
- `tests/static_regular.cpp`、`tests/static_builder.cpp`：归档到 `tests/retired/`。
- `benchmarks/plan_digest.cpp`：归档到 `benchmarks/retired/`。
- `build.mk`/`CMakeLists.txt` 中 `init_legacy`/`mmio_legacy`/`compile_legacy`/
  `poe_legacy_*`/`scale_legacy` 目标。

### 2.2 新增/修改

- `FormulaBuild`（`include/regfw/static_layout.hpp`）：metadata-only 遍历，产出
  bindings/scopes/identities/mounts；无 word/candidate/page/redirect 存储。
- `FormulaDecoder`（`src/detail/runtime_impl.hpp`）：run 索引、4 KiB 稀疏页目录、
  zone bisect、reserve 遮蔽、`fill_entry`、`fast_access`、`prepare_fast_paths`。
- `formula_decode_interleaved2`：gcd 归约 + 模逆 + `__int128` 无 UB 求交。
- reserve：半开 `[base,base+size)`，`ReserveRangeOverflow`，运行期按优先级遮蔽。
- `validate_overlaps`：绝对地址区间 walker，保留原有冲突文本。
- identity 快速路径：finalize 预计算目标 eligibility，移除每次访问的 bank0 解码。
- **动态布局合并**：`src/address_view.cpp` 的 `ViewCompiler` 不再生成逐 word 候选/索引；
  每个映射元素安装为 `FormulaDecoder::Mount`（Scalar 数组按 `occupied` 共享，
  `Binding` 携带元素线性索引），`install_dynamic` 复用 zone/page/run、reserve 遮蔽和
  shadow warning。`compiled_views_` 在动态布局中保持为空。
- `FormulaDecoder::Binding` 增加 `linear`；`fill_entry` 以 `binding.linear+linear` 生成
  target/callback index；`available` 在跨实体/overlay 边界处截断为 4，避免 8 字节
  访问跨越不同元素；`fast_access` 的 Scalar 指针按 binding 线性索引定位。
- 测试：`formula_geometry`、`formula_golden`、`formula_diff` 两路差分、
  `static_layout`/`symbolic_layout` 公式化、`address_view` 动态 overlay/slice/lifetime。
- GCC 移植：`build.mk` 按编译器选择 `-fconstexpr-steps`/`-fconstexpr-ops-limit`；
  `tests/formula_diff.cpp` 删除 GCC `-Werror` 命中的未使用 `digest`；
  `examples/pf_vf_layout.hpp` 的 QUEUE 声明收为单行，消除 GCC/Clang 对宏
  `source_location` 行号不同造成的生成文档漂移；`benchmarks/measure_formula.py`
  也按编译器选择 constexpr 选项并支持 `REGFW_BENCH_BUILD`。
- 文档：`docs/FormulaBackend.md`、`docs/Migration.md`、`VALIDATION.md`、`CHANGELOG.md`、
  `README.md`、本报告。

## 3. 验证

| 项目 | 结果 |
|---|---|
| 本地 build.mk 套件 | 27/27 PASS |
| CMake + CTest（AppleClang 21） | 32/32 PASS（含示例、PCH、compile-fail、文档） |
| CMake + CTest（GCC 16.2） | 32/32 PASS（strict `-Wall -Wextra -Werror`） |
| Clang `-Wall -Wextra -Werror` | PASS |
| GCC 16.2 `-Wall -Wextra -Werror` | build.mk 27/27 PASS |
| ASan+UBSan 全套 | 27/27 PASS，无报告 |
| 旧符号残留 | include/src 无匹配 |
| golden 解码 | 1321/1321 word 字段一致 |
| dynamic-vs-formula 随机差分 | 20k 次读写一致 |
| reserve/绝对重叠 | formula_geometry PASS |
| 动态布局统一 | `address_view` PASS，无逐 word `compiled_views_` 索引 |

未执行：GCC 13/14、QEMU、真实 POE/DAG 业务源码、hlog 插桩 ASan。GCC 16.2 已在本机执行并通过。

## 4. 性能

Apple clang 21，`-O1`，`benchmarks/results-0.35.0/formula-bench.md`：

| 指标 | 0.34 旧后端 | 0.35（静态 / 动态） |
|---|---|---|
| POE 编译 | 20.1s / 1003MiB / 3882KiB | 8.68s / 623MiB / 918KiB |
| DAG 编译 | 2.39s / 397MiB / 1108KiB | 1.2s / 224MiB / 96KiB |
| 8 VF/262144 word 编译 | 104.7s / 3010MiB / 40574KiB | 0.98s / 216MiB / 76KiB |
| DAG 6 init | 1.073ms / 13.0MiB | 0.952ms / 9MiB |
| Scalar32 hot read | 21.6ns | 44.3ns / 48.2ns |
| mapped read | 71.7ns | 113.9ns / 119.4ns |
| random read | 30.8ns | 48.5ns / 51.8ns |
| callbacks hot read | 36.6ns | 71.4ns / 77.0ns |

编译期/峰值 RSS/静态元数据显著下降；动态比静态公式慢约 5–9%，两者相对 0.34 动态
快路径仍慢约 2.1–2.2x，5% 目标未达成。没有用“内存降低”来接受该回退；剩余优化点在
identity 目标解码、mapped 路径的两次 mount 查找，以及动态逐元素 mount 的 zone 选择。

## 5. 未完成/风险

1. MMIO 热路径未达 5%；需要进一步合并 `fast_access`/`mapped_scalar` 的查找，或为
   identity 目标建立按窗口（非按 word）的缓存。
2. GCC 13/14 未在本机执行；本机 GCC 16.2 已通过 strict build 与 CTest 32/32，Linux/旧 GCC 仍需复跑。
3. 真实 POE/DAG 业务源码未接入；仅有基准布局。
4. `tests/retired/` 保留历史测试源码，不参与构建，编译需要旧后端。
5. 动态布局逐元素安装，元素数很大时 mount 数随之增大；当前 8 VF/262144 word 基准
   仍可接受，但尚未做百万级元素压力测试。

## 6. 文件

源码：`include/regfw/`、`src/`、`tests/`、`benchmarks/`、`CMakeLists.txt`、`build.mk`。
文档：`README.md`、`CHANGELOG.md`、`VALIDATION.md`、`docs/FormulaBackend.md`、
`docs/Migration.md`、本报告。
归档：`work/regfw-0.35.0-ds.tar.gz`。
