# regfw 0.34.0 DS 版交付报告

日期：2026-10-31  
工作树：`work/regfw-0.34.0-ds`（从 0.33.3 基线复制，独立实现 Formula Decoder；非 git 仓库）  
本机：macOS arm64，Apple clang 21.0.0，C++20，`-O2`（编译期基准用 `-O1`，与 0.33.3 基线一致）

## 1. 结论摘要

DS 版完成公式化地址解码后端，**功能兼容性通过本机全部 27 项测试**，包括：

- 编译期旧 `StaticWord` 逐 word 与公式解码逐字段对拍；
- 运行期 dynamic / legacy / formula 三模式 MMIO、回调、Access Group、identity、pre_access、快照、宽寄存器部分事务差分；
- 真实 POE 形状（VF repeat、PF-shared overlay、512-bit GET_GROUP、双 ACC 地址族、混合宽度）的编译与运行。

编译期和静态元数据收益显著：

- POE 型 TU：20.1 s / 1003 MiB / 3882 KiB → **10.4 s / 658 MiB / 918 KiB**
- 8 VF × 262,144 word：104.7 s / 3010 MiB / 40,574 KiB → **0.96 s / 214 MiB / 76 KiB**
- DAG 型 TU：2.39 s / 397 MiB / 1108 KiB → 1.48 s / 223 MiB / 96 KiB
- DAG 6 实例安装：1.073 ms → 0.728 ms（finish 229.5 µs → 18.9 µs）

**未达标项**：MMIO 热路径仍慢于旧后端，Scalar32 hot 42.3 ns vs 21.9 ns（约 1.9×），mapped 114.9 ns vs 74.7 ns（约 1.5×），random 48.7 ns vs 28.5 ns（约 1.7×）。任务书的 5% 初步门槛没有达到；本次不把“内存降低”当作性能达标，原因和后续方案写在 `docs/FormulaBackend.md` 第 7 节。

## 2. 与 GLM 版的关系

DS 树从 GLM 工作树复制，保留其测试/基准/接入骨架，但重新实现了核心计划与解码：

| 维度 | GLM 版 | DS 版 |
|---|---|---|
| 非单调二维数组 | `static_sparse_order` 逐元素记录 + 二分 | `Interleaved2`：gcd 归约 + 模逆求交，O(1) 算术；只对不可解/高维形状回退 Table |
| 线性索引 | `formula_element_offset` 按 stride 排序维迭代，转置布局有错位风险 | 增加声明顺序 `strides_decl/extents_decl/multiplier_decl`，正反向都用声明维 |
| 单寄存器分类 | 仅 rank==0 才 Scalar，单寄存器实际是 count=1 Monotone | `count==1` 即 Scalar；标量数组也允许直接 scalar 指针 |
| 运行时分发 | zone 二分 + 候选 | zone 二分 + 连续标量 run 索引 + 4 KiB 稀疏页目录；命中失败才回退 |
| 安装期冲突检查 | 对重叠 mount 逐元素展开 | 区间游走，只比较实际可能相交的元素 |
| Table 上限 | 无显式上限 | 4096 条记录上限，超过构建期报 `UnsupportedGeometry`，禁止静默回退逐 word |
| 快速路径 | 每个访问重新生成 `CompiledAccess` | 常见 Scalar32 不构造 `AliasEntry`；identity/回调等仍走完整上下文 |

编译期结果与 GLM 接近（GLM：DAG formula 1.4 s/98,048 B，POE formula 10.6 s/937,848 B；DS：1.48 s/96 KiB，10.4 s/918 KiB）。DS 的差异主要在数组反解、诊断边界和直接标量快路径；MMIO 差距仍未解决。

## 3. 代码变更清单

### 新增

- `include/regfw/detail/formula_plan.hpp`：`FormulaArraySpec` / `FormulaMountSpec` / `FormulaPlan` / 五类解码算法与 helper。
- `src/formula.cpp`：`FormulaInstaller`、plan→decoder 安装、runs/page 目录、区间冲突检查、Table 上限诊断。
- `tests/formula_diff.cpp`：编译期 decode 差分 + 三后端运行期差分。
- `benchmarks/formula_scale.cpp`：8 VF/262,144 word 规模基准。
- `benchmarks/measure_formula.py`：编译/安装/MMIO 测量脚本（RSS 已改为编译器 `maximum resident set size`）。
- `docs/FormulaBackend.md`：设计、兼容性、测试、性能与迁移说明。
- `DELIVERY-REPORT-0.34.0-DS.md`：本报告。

### 修改（仅地址解码/安装/接入）

- `include/regfw/static_layout.hpp`：`formula_run`/`compile_formula_layout`、数组分类、span、cap、`REGFW_LEGACY_STATIC_WORDS` 分支。
- `include/regfw/detail/static_plan.hpp`：`ElementRecord`。
- `include/regfw/runtime.hpp`：`formula_`、`view_entry`、friend；公开方法签名未变。
- `include/regfw/types.hpp`：仅为 `FormulaInstaller` 增加 friend；公开类型未变。
- `src/detail/runtime_impl.hpp`：`FormulaDecoder`、`find_mount`/`fill_entry`/`fast_access`、run/page 索引。
- `src/runtime.cpp`：`resolve_static`/`resolve_metadata`/`finalize` 的公式分支；读写快路径改走 `fast_access`；执行引擎、事务、回调未改。
- `src/layout_report.cpp`：Full 报告在公式模式下枚举解码 word。
- `benchmarks/mmio.cpp`、`benchmarks/poe_layout.cpp`：静态基准入口与 POE 形状。
- `tests/static_regular.cpp`：公式/旧后端断言。
- `CMakeLists.txt`、`build.mk`、`README.md`、`CHANGELOG.md`、`VALIDATION.md`：版本、构建、说明。

### 保持不变（用于证明未重写业务）

- `src/static_layout.cpp` 旧安装器未改。
- `REG`/`REG_BUNDLE`/`REG_LAYOUT`/`REG_LAYOUT_IMPL` DSL、Entity/Binding/Storage、RegView、TransactionPolicy、SplitRead/Write、回调、PreAccess、Access Group、IdentityResolver、Snapshot v2、Reset、日志、动态布局。
- QEMU 侧 MMIO API 与地址语义。

## 4. 测试结果

本机 `make -f build.mk -j4 run-tests`：**27/27 exit 0**，最后运行日志 `/tmp/tests-final.log`。覆盖：

- `formula_diff`：旧 word 表每个条目 vs 公式 decode；洞 vs 未命中；三后端 MMIO/回调/Access Group/identity/snapshot/部分宽事务。
- `static_regular` / `static_layout` / `static_builder`：0.33.3 的静态差分、宽度、稀疏、非单调、覆盖、identity 预算。
- `snapshot`、`mapped_logging`、`layout_separate`、`bundle_light`、`poe_compiled`、`pf_vf`、`layout_report`、`fast_path`。

**未执行**：GCC 13/14 Release、ASan/UBSan、完整 CTest（本机无 ctest/GCC）。这是验收缺口，不能宣称“全部 CTest 通过”。

## 5. 基准原始结果

- `build-local/formula-bench.json`
- `build-local/formula-bench.md`
- 编译期 RSS 用 `/usr/bin/time -l` 的 `maximum resident set size`；脚本已修正旧脚本误读 `peak memory footprint` 的问题。

## 6. 打包

交付归档：`work/regfw-0.34.0-ds.tar.gz`（排除 `build-local/`、`logs/`、`third_party/hlog/target/` 等构建产物）。解包后按原 CMake 方式构建；本机可用 `make -f build.mk run-tests`。
