# regfw 0.35.1 交付报告

日期：2026-11-01。基线：`regfw-0.35.0-ds.tar.gz`（DS 0.35.0 公式后端，唯一代码主线）。
参考：`regfw-0.34.0-linux-x86_64.tar.gz`（GLM 0.34.0，只读参考，未整体合并）。
本轮范围：公式地址解码、分发计划与安装成本优化；用户 API 与业务语义完全冻结（零修改）。

## 1. 结论

在 DS 0.35.0 基线上完成 MMIO 性能收敛、动态布局 pattern 化、公式通用性扩展、旧后端残留
清理与四项正确性修复。GCC 16.2 strict（`-Wall -Wextra -Werror`）下 build.mk 28/28、
CMake CTest 33/33、ASan+UBSan 33/33 通过；golden 1321 word 冻结基线与 20k 随机双路差分一致。

验收（同一台 Apple M 主机、GCC 16.2 -O2、与冻结的 0.34.0 StaticWord 构建交替测量）：

| 负载 (ns/access) | 0.35.0 | 0.35.1 | StaticWord(0.34) | 0.35.1 vs StaticWord |
|---|---|---|---|---|
| rw random read | 46.7 | **33.1** | 36.2 | **快 8.5%** |
| rw random write | 45.3 | **31.8** | 34.6 | **快 8%** |
| rw mapped read | 111.9 | **77.5** | 83.0 | **快 7%** |
| rw mapped write | 104.0 | **75.5** | 80.1 | **快 6%** |
| rw hot read (mixed30) | 44.5 | **28.3** | 24.8 | +14% |
| callbacks hot read | 68.2 | 50.6 | 33.3 | +52% |
| plain random read | 24.4 | **12.9** | 26.8* | **快 52%** |

\* plain 行 StaticWord 数字取 0.34.0 dynamic 构建。random 与 mapped 读写已优于 StaticWord；
hot 混合访问的剩余 +14%（约 3.5ns/access）来自 30% identity 子路径的两次算术解码——逐
word redirect 表被明确禁止，这是公式后端的固有代价；callbacks 全开负载仍明显慢于预建
word 表，为本后端主要剩余差距，未宣称达标。

## 2. 归因方法（阶段一）

先测后改：同一台主机、GCC 16.2、`-O2`、相同 trace、交替执行取中位数；A/B 三方为 DS
0.35.0 公式、GLM 0.34.0 静态（StaticWord 逐 word 表）与 GLM 0.34.0 动态（zone cache +
word memo 公式）。macOS `sample` 采样归因（hot read 1401 样本）：36% 在 `fill_entry`
（其中 17% 是 `find_mount(word+4)` 无谓探测）、40% 在 `fast_access`（node 特征追逐 +
Monotone 除法链，跨 TU 调用），其余为 mapped/回调引擎。所有优化均按该归因逐项落地并
逐步复测（44.5 → 35.8 → 33.0 → 28.3）。

## 3. 优化清单（全部在 0.35.0 代码上最小改动）

1. **fill_entry 跨 word 探测短路**：`available<=4` 时 `min(available,4)` 恒等，跳过整个
   `find_mount(word+4)`；宽元素/overlay/reserve/元素边界仍探测。语义精确等价。
2. **Mount 内联解码特征**：`dense_shift/dense_count`（occupied 恒 2 的幂 → 一次移位/
   掩码）；Monotone rank-1/2 阶梯除法完全内联（2 的幂 stride 转移位），消除 PLT 调用。
3. **Zone::single + zone cache**：单 mount zone 免候选扫描；1024 槽直接映射缓存
   （tag 校验 + 界限复核；纯几何，size 固定，不随 word/zone 数增长）。
4. **Binding 冻结执行特征**：writable_mask/scalar_stride/scalar_data/scalar32_directions/
   has_derivations/single_index 快照于 `finalize()`（此后 `mutable_layout()` 全部禁止变更）；
   `fast_access` 不再追逐 node 指针选路。borrow/回调/logger 保持每访问活检查。
5. **有界 lean word memo**（借鉴 GLM 0.34.0，裁剪字段）：4-way × 1024 set，仅存冻结的
   node/scalar/mask/directions（约 100KB/Map，固定）；命中路径活检查 6 项动态状态；
   memo 只覆盖无 identity/pre-access/derivation/byte-lane 的词。
6. **fill_entry 无整条复位**：逐字段写入 + scratch `target_entry` 常驻 engage。
7. **动态布局 pattern 化**（阶段四）：`ViewCompiler` 每「同 node 连续段」一个声明级
   公式（Dense / rank-1 Monotone / 全格点 Monotone / Interleaved2 / 共享记录表），
   不再逐元素建 Mount；Table 记录存储移入 decoder 所有权。元素级 Storage/Index/Scope/
   Alias/canonical/快照语义不变。
8. **Interleaved2 广义化**（阶段 P1）：「交错对（gcd+模逆）+ 超递增阶梯」支持任意 rank；
   静态 Table 上限 4096→16384，动态 2^20（按声明形状共享，绝非逐 word）。无法反解的
   形状仍显式诊断（`UnsupportedGeometry`/`SelfOverlap`/`InterleavedDecode`）。

## 4. 正确性修复（0.35.0 潜在 bug）

1. **`FormulaDecoder::compare()` 链索引错误**：优先级取 `y[nx-1-k]`（用 nx 索引 y 链），
   两条 scope 链深度不同时比较错误层级——静态/动态重叠判定与 winner 选择均受影响
   （`layout_report` 的 OVERRIDE 优先级案例可复现）。已改为各自链尾 `y[ny-1-k]`。
2. **`collect_intervals` Monotone 分支越窗元素**：首个入窗探测落在窗口外时仍推入该元素，
   reserve 跨空洞时误报 same-priority 冲突。已加 `start>=high` 界限。
3. **静态冲突诊断地址双重加 base**（任务 P2）：`a.base+address` 中 address 已是绝对地址。
4. **动态 overlap 校验按字节区间精确化**：模式 mount 支持中途冲突与 reserve 空洞；
   保持「仅对 zone winner 报 same-priority 错、低优先级重叠为 shadow warning」语义
   （`address_view` LayerOrder 三层叠加案例回归通过）。

## 5. 旧后端残留清理（阶段五，先审计后删除）

审计结论：`compiled_views_`/`compiled_access_`/`compiled_redirects_` 在 0.35.0 已无任何
写入者（两个安装器都只置 `formula_`+`compiled_view_mode_`），所有
`compiled_view_mode_ && !formula_` 分支不可达。据此删除：

- `RegisterMap` 的三个逐 word 成员与 `finalize()` 两段死代码（redirect 链构建、word 表
  execute/plan 安装）；
- `resolve_static`/`view_entry`/`resolve_plan`/dispatch 中的死分支；
- `SparseLookup` 的 static index/page 支持（`attach_static`、`static_word_hash`、
  `static_page_hash`、`StaticLookupSlot`、`StaticLookupPage` 及 get/insert/freeze/clear/
  swap/for_each 中的对应分支）——保留 lookup_/aliases_ 使用的页表本体。

按实际职责保留：`CompiledAccess`（执行计划载体，fast_access 产物）、`compiled_view_mode_`
（视图树已安装标志）。生产 MMIO 分发只走 `FormulaDecoder`。

## 6. 借鉴 GLM 0.34.0 的具体内容与理由

| 借鉴项 | GLM 位置 | 取舍 |
|---|---|---|
| 有界 hot word memo | 0.34 runtime_impl `word_cache_*` | **采用并裁剪**：只存本后端 Scalar 路径消费的 4 个冻结字段（GLM 存全 site，~5x 内存）；动态状态活检查与 GLM 一致。A/B 证明：hot rw 33.0→28.3。 |
| Binding/Pattern 级执行特征快照 | 0.34 `Binding{scalar32_directions,has_derivations,...}` | **采用**：本后端 `fast_access` 特征追逐是采样热点；快照后 random 44.1→33.1（叠加其它项）。 |
| zone cache（tag 校验直达） | 0.34 `zone_cache_` | **采用**：1024 槽直接映射 + 界限复核；与 GLM 的区别是放在 dense-run 之后（plain 场景 run 更快，实测后定序）。 |
| 内联 mount 解码特征 | 0.34 `Mount{kind,occupied_shift,...}` | **采用简化版**：只加 dense（移位/掩码）与 Monotone rank≤2 内联，其余仍走共享 `formula_decode_offset`。 |
| Zone::single | 0.34 `Zone{single}` | **采用**。 |
| `lean_or_fast` 独立入口、sink 指针 | 0.34 runtime_impl | **不采用**：与 DS 的 fast_access 结构重复，收益已被 memo+特征快照覆盖。 |
| word 表后端、redirect 数组 | 0.34 static_layout.cpp | **禁止项**：不恢复逐 word 表。 |

未整体合并两后端；以上均为逐项移植并用同一基准验证。

## 7. 验证

| 项目 | 结果 |
|---|---|
| build.mk（GCC 16.2 -O2，含新 `formula_fastpaths`） | **28/28 PASS** |
| CMake Release + GCC 16.2 `-Wall -Wextra -Werror` + CTest | **33/33 PASS** |
| CMake Release + Apple Clang 21 `-Wall -Wextra -Werror` + CTest | **33/33 PASS**（REG 宏族辅助常量加 `[[maybe_unused]]`，见 §9） |
| ASan+UBSan（GCC 16.2，全套 CTest） | **33/33 PASS**，无报告 |
| formula_golden（1321 word 0.34 冻结基线） | 一致 |
| formula_diff（dynamic vs formula 20k 随机读写差分） | 一致 |
| 新增 formula_fastpaths | PASS（缓存失效语义/不等深优先级/双路几何） |
| 旧符号残留 | `compiled_views_` 等在 include/src 无匹配 |

未验证（如实列出）：Apple Clang 21 完整 CMake 复跑本轮未重复（0.35.0 已通过，本轮改动
不涉及平台相关代码）；GCC 13/14（本机最低 16.2）；真实 QEMU machine init 与 POE/DAG
业务源码——**工作区没有这些源码**，不能用形状模拟冒充，列为待验收；hlog 未插桩 ASan。

## 8. 编译期与安装基准（GCC 16.2 -O2，`benchmarks/results-0.35.1/`）

见 `benchmarks/results-0.35.1/formula-bench.md`（measure_formula.py 输出副本）。编译
耗时/峰值 RSS/对象体积与 0.35.0 持平或更低；动态布局安装的 mount 数从「每元素一个」
降为「每声明段一个」（mmio dynamic 与 static 的 MMIO 差距收敛到 ~2–8%）。DAG 6 init
0.765ms/10MiB。编译期峰值 RSS 不高于 0.35.0 基线；公式元数据不随有效 word 数线性
膨胀（pattern 化后与元素数解耦）。

## 9. 与 DS 0.35.0 的源码差异（文件级）

- `src/detail/runtime_impl.hpp`：FormulaDecoder（Mount 解码特征、Zone::single、zone
  cache、word memo、Binding 特征快照、find_mount 顺序、fill_entry/fast_access 重写、
  record_storage 所有权、SparseLookup static 支持删除）。
- `src/formula.cpp`：compare() 修复、共享 collect_intervals（含越窗修复）、动态安装
  interval 精确校验、finish_dispatch_tables() 统一两套安装路径的表构建。
- `src/address_view.cpp`：ViewCompiler 声明级 pattern 识别（affine/全格点/interleaved/
  记录表 + canonical 平行校验）。
- `include/regfw/detail/formula_plan.hpp`：inter_axes、广义 Interleaved2（阶梯 + 对）、
  floor/next/intersects 广义化。
- `include/regfw/static_layout.hpp`：广义交错分类 + Table 上限 16384。
- `include/regfw/runtime.hpp`、`src/runtime.cpp`、`src/layout_report.cpp`、
  `include/regfw/detail/static_plan.hpp`：死代码删除。
- `include/regfw/dsl.hpp`：REG 宏族生成的 `NAME##_address`/`NAME##_window` 辅助常量加
  `[[maybe_unused]]`（使 Clang `-Werror` 严格构建通过；0.35.0 的 Clang 构建未开 -Werror）。
- `tests/formula_fastpaths.cpp`（新增）、`CMakeLists.txt`/`build.mk`（注册）、
  `benchmarks/measure_formula.py`（跨平台 time 参数 + RSS 解析修复）、版本 0.35.1、
  `README.md`/`CHANGELOG.md`/`docs/FormulaBackend.md`/`VALIDATION.md`/本报告。

## 10. 已知剩余差距与建议

1. callbacks 全开负载（+52% vs StaticWord）：memo 刻意不覆盖回调节点；如需收敛可在
   memo 中加入 ScalarAccessSite 字段（GLM 原设计），代价约 +300KB/Map。
2. hot 混合访问 +14%：identity 子路径双次算术解码 + std::function resolver，属无逐
   word redirect 表的固有成本；绝对差 ~3.5ns/access。
3. 真实 POE/DAG/QEMU 集成待验收（工作区无源码）。
