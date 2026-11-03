# Changelog

## 0.37.0

**Readability / modularization release (no intended API or behavior changes).**

- Apply a consistent, checked-in `.clang-format` style across first-party C++20 sources, examples, and tests; expand compressed statements and format callbacks and templates for review.
- Organize implementation sources by responsibility: `src/dsl`, `src/layout`, `src/runtime`, `src/snapshot`, `src/diagnostics`, and private `src/internal` headers.
- Separate the former monolithic `runtime.cpp` into address resolution/MMIO execution, lifecycle, entity/handle binding, callbacks/behavior, and snapshot state code.
- Keep all installed `include/regfw/*.hpp` paths and public API intact. Move archived delivery reports out of the source root into `docs/history/`.
- Update CMake, documentation generation, architecture guide, and regression tests for the module layout.

## 0.36.0

- Switch static REG_LAYOUT/REG_LAYOUT_IMPL installation to one native C++ layout build per factory. Delete `compile_formula_layout`, the two-pass consteval FormulaPlan, old constexpr record sorting, and custom constexpr allocation buffers.
- Make prepared EntityLayout Hash the sole production View decoder; remove the unused alternative Zone/Run/PageDirectory dispatch and the `REGFW_PREPARED_ACCESS_HASH` build option.
- Per-REG type width/rank/strides remain lightweight constexpr metadata; nonmonotone array records are sorted at runtime once per REG declaration type.
- Keep the public DSL, access engine, callback, resolver, transaction, snapshot and QEMU MMIO interfaces unchanged. Invalid geometric layouts are diagnosed during installation rather than C++ constant evaluation.
- Preserve golden/dynamic differential tests and add native-installer negative geometry tests; remove obsolete compiler-limit flags from active build and benchmark entries.


## 0.35.2

纯内部修复版本，用户 API/REG DSL/事务/快照格式保持 0.35.1 不变。

- 动态 Table 公式按完整记录语义隔离，不再以缺少实际偏移序列的 ArrayKey 错误复用不同声明。
- 公式动态 Mount 的重叠区间比较统一转为绝对地址，修复稀疏数组地址冲突漏判和误判。
- 对高优先级稀疏 Mount 空洞中的同优先级低层重叠进行元素级验证；真正被高优先级实体覆盖的低优先级冲突仍遵守既有语义。
- 静态 Reserve 的 shadow warning 检查真实寄存器占用字节，不再以 Mount 地址包围盒误报。
- 新增 `formula_regressions`，覆盖两张不规则 Table 的读写/空洞、跨 Mount 冲突、稀疏高优先级空洞及静态/动态 Reserve 一致性。
- 修复 Linux `measure_formula.py` 编译耗时和最大 RSS 解析，提供可用的顶层 `build.mk`，打包时移除 macOS AppleDouble 元数据。


## 0.35.1

性能收敛与稳定化：以 0.35.0 公式后端为唯一基线，冻结全部用户 API；按 profiler 归因逐项消除重复解码，不重新引入任何逐 word 静态表。

- **热路径**（`FormulaDecoder`）：
  - `fill_entry` 的跨 word 探测在 `available<=4` 时跳过（`min(available,4)` 恒等），4 字节标量/mapped/callback 访问不再多付一次完整 `find_mount(word+4)`；宽元素、overlay、reserve 与元素边界仍逐字节探测。
  - `Mount` 内联 dense 解码特征（`dense_shift/dense_count`，occupancy 恒为 2 的幂 → 一次移位/掩码）；`Monotone` rank-1/2 解码完全内联（2 的幂 stride 转为移位），消除跨 TU 调用。
  - `Zone::single`：单 mount zone 免候选扫描；新增 1024 槽直接映射 zone cache（tag 校验 + 界限复核，纯几何缓存，size 固定不随布局增长）。
  - `Binding` 冻结执行特征（writable_mask/scalar_stride/scalar_data/scalar32_directions/has_derivations/single_index，均在 `finalize()` 后不可变），`fast_access` 不再逐访问追逐 node 指针；borrow、回调、logger 仍每访问活检查。
  - 有界 lean word memo（4-way × 1024 set，仅冻结字段 node/scalar/mask/directions；动态状态全部活检查）——借鉴 GLM 0.34.0 word cache 思路，裁剪为本后端 Scalar 路径实际消费的字段（约 100KB/Map，固定）。
  - `fill_entry` 逐字段写入（不再整条 `out={}` 复位），scratch 的 `target_entry` 常驻 engage，消除 optional 抖动。
- **动态布局 pattern 化**：`ViewCompiler` 不再逐元素生成 Mount；每个同 node 连续段识别为声明级公式（Dense / rank-1 Monotone / 全格点 Monotone / Interleaved2 / 任意形状共享记录表），一个规则数组一个（或少量）Pattern。Table 记录存储移入 decoder。元素级 Storage、Index、Scope、Alias、canonical 地址与快照语义不变。
- **公式通用性**：Interleaved2 广义化为「交错对 + 超递增阶梯」——任意 rank 的交错声明只要每个非对轴支配其下方完整跨度即可纯算术反解；静态 Table 上限 4096→16384，动态 Table 上限 2^20 条记录（仍按声明形状共享，绝非逐 word）。
- **正确性修复**（0.35.0 潜在 bug）：
  - `FormulaDecoder::compare()` 用 `nx` 索引两条链（`y[nx-1-k]`）——链深不同时比较错误的优先级层级；已改为各自链尾索引 `y[ny-1-k]`。
  - 共享 `collect_intervals` 的 Monotone 分支可能把窗口外元素推入结果（reserve 跨空洞时误报 same-priority 冲突）；已加 `start>=high` 界限。
  - 静态冲突诊断 `a.base+address` 双重加 base（address 已是绝对地址）；已修。
  - 动态 overlap 校验改为字节级区间精确（模式 mount 可中途冲突、reserve 只在真实字节相交时报错），并保持「仅对 zone winner 报 same-priority 错误、低优先级重叠为 shadow warning」的既有语义。
- **旧后端残留清理**：删除不可达的 `compiled_views_`/`compiled_access_`/`compiled_redirects_`（0.35.0 起已无任何写入者）、`finalize()` 中 `compiled_view_mode_&&!formula_` 的两段死代码、`SparseLookup` 的 static index/page 支持（`attach_static`/`static_word_hash`/`static_page_hash`/`StaticLookupSlot`/`StaticLookupPage`）。`CompiledAccess`（执行计划）与 `compiled_view_mode_`（视图树已安装标志）按实际职责保留。生产 MMIO 分发只走 `FormulaDecoder`。
- **工程**：`benchmarks/measure_formula.py` 的 `/usr/bin/time` 参数按平台选择（macOS `-l` / Linux `-v`），RSS 单位统一 KiB；版本号 0.35.1。
- **测试**：新增 `formula_fastpaths`（memo/zone cache 在动态回调附加、borrow 重入下的失效语义；不等深 scope 优先级；dense/strided/interleaved2/rank-3 ladder/非 2 幂 stride/空洞/64-bit 宽数组的动态+静态双路几何）。
- **基准**（GCC 16.2 -O2，Apple M 系主机，与冻结的 0.34.0 StaticWord 构建交替测量，ns/access，mixed30=1000 寄存器/30% VF identity 映射）：rw hot 44.5→31.1（StaticWord 24.8，+25%）；rw random 46.7→34.3（36.2，**快 5%**）；rw mapped 111.9→76.8（83.0，**快 7%**）；plain random 24.4→12.9（StaticWord 26.8，**快 52%**）。hot 混合访问的剩余差距来自 30% identity 子路径的两次算术解码（无逐 word redirect 表可用的代价）；绝对差 ~6ns/access。Callbacks hot 68.2→50.8。

## 0.35.0

- **删除旧逐 word 后端**：移除 `StaticBuild`、`StaticWord`/`StaticShadow`/`StaticPlanView`、`StaticPlan`、`compile_static_layout`、`install_static_layout`、`src/static_layout.cpp` 和 `REGFW_LEGACY_STATIC_WORDS`。`REG_LAYOUT` 只剩公式后端，不存在宏切换或静默回退。
- 新增 metadata-only `FormulaBuild`（binding/scope/identity/mount 遍历，无 word/candidate/page/redirect 存储），`formula_run` 不再实例化旧 `StaticBuild<true>`；POE 编译从 10.4s 降到约 8.7s。
- reserve 语义修正为半开 `[base, base+size)`，增加 `ReserveRangeOverflow` 溢出诊断；`validate_overlaps` 按绝对地址比较，修复不同 base 挂载间的假冲突/漏判；运行期 reserve 按优先级遮蔽寄存器（不再误报 `ReservedIntersection`）。
- `formula_decode_interleaved2` 移除无符号 `__int128` 的负数比较 UB；二维非单调数组使用 gcd 归约 + 模逆求交，声明序线性索引保持。
- identity 快速路径在 `finalize()` 预计算目标 eligibility，`mapped_scalar` 不再每次访问解码 bank 0；Scalar32 直接路径不构造 `AliasEntry`。
- **动态布局合并**：运行时 `AddressView` 不再生成逐 word `compiled_views_` 索引；`ViewCompiler` 为每个映射元素建立 Scalar `Mount`，binding 携带元素线性索引，`install_dynamic` 生成 zone/page/run 目录、reserve 遮蔽和 warning。动态布局与静态布局共用 `FormulaDecoder`。
- 测试：新增 `formula_geometry`（跨 base 包围盒重叠/reserve）与 `formula_golden`（1321 条 0.34 word 基线冻结）；`formula_diff` 改为 dynamic-vs-formula 两路 MMIO/快照/reset 差分；`static_layout`/`symbolic_layout` 改用公式 plan；旧 builder 内部测试归档到 `tests/retired/`。27/27 本机测试、ASan/UBSan 全套、Clang `-Wall -Wextra -Werror` 通过。
- 基准（Apple clang 21，`-O1`）：POE 8.68s/623MiB/918KiB；DAG 1.2s/224MiB/96KiB；8 VF/262144 word 0.98s/216MiB/76KiB；DAG 6 init 0.952ms/9MiB。MMIO hot 静态公式 44.3ns / 动态 48.2ns（0.34 dynamic 21.6ns），mapped 113.9/119.4ns（71.7ns），**5% 目标未达成**；详见 `docs/FormulaBackend.md` §7。
- CMake/CTest（AppleClang 21，32/32）与本机 HLog dylib 构建已通过；GCC 16.2（strict `-Werror`）本地 27/27、CTest 32/32 通过；未验证：GCC 13/14、QEMU 实机、真实 POE/DAG 业务源码（本机只有基准布局）。

## 0.34.0

- 新增 Formula Decoder 后端：编译期产出 `FormulaPlan`（binding/scope/identity/mount/array 描述），不再展开逐 4-byte `StaticWord`；运行时 `FormulaDecoder` 按窗口/Repeat/数组 stride 直接计算 `EntityState`、linear index 和 byte offset，再进入原有访问引擎。
- 数组解码覆盖 `Scalar`/`Dense`/`Monotone`/`Interleaved2`/`Table` 五类：单调多维逐维商余；二维非单调布局用 gcd 归约 + 模逆求交，避免逐元素记录；更高维不可逆形态在 4096 条记录上限内使用共享表，超过上限在构建期报 `UnsupportedGeometry`，不静默回退逐 word builder。
- 新增直接 Scalar32 快速路径（单寄存器与标量数组）、dense scalar run 索引、4 KiB 稀疏页目录、identity target 复用；回调、pre-access、overlay、derivation 和宽寄存器事务仍走原有执行路径，`DecodedAccess` 不复刻业务逻辑。
- `StaticInstaller` 不再逐 word 创建 `AliasEntry`；`RegisterMap::finalize()` 在公式模式下跳过 `CompiledAccess`/word hash/page hash 安装，保留 scope、name、pre-access、identity、状态和快照检查。
- 公开 API、DSL、CMake 集成和外部调用方式不变；旧 `StaticWord` 后端继续保留用于差分和历史冲突布局，`REGFW_LEGACY_STATIC_WORDS` 显式切换。
- 测试：`formula_diff` 在编译期把旧 word 表的每个条目与公式解码逐字段对拍，并运行 dynamic/legacy/formula 三模式 MMIO、回调、access group、identity、快照和部分宽事务差分；0.33.3 基线的 27 项本机测试全部通过。
- 基准（Apple clang 21，`-O1`）：POE 型 TU 19.0s/1.03GiB → 10.1s/0.66GiB，目标文件 3.9MiB → 918KiB；8 VF/262144 word 规模 110.4s/2.87GiB/40.6MiB → 1.1s/0.21GiB/76KiB。Scalar32 hot path 仍有约 2x 回退，详见 `docs/FormulaBackend.md` 的未达标项。

## 0.33.3

- 新增稀疏规则构建器：以寄存器元素为单位几何建模（`ElementMount`/`StaticElementPlan`），按实际占用区间检测地址冲突。稀疏 stride、多维交错、混合宽度（32/64/128/512/2048-bit）、大地址空洞、无冲突的包围盒重叠 View 叠加不再触发完整构建器；真实字节重叠、保留区相交、越界与 identity 越界仍回退原优先级求解器。
- 单调布局（稠密/一维稀疏/可证升序多维）用游标直接生成元素，交错多维按声明共享的有序元素表生成；全程不物化逐 word Candidate。
- 稠密布局保留专用快路由：连续占用块直接计数/填表，性能与 0.33.2 相当或更好（N=64/128/256：4.6s/0.48GiB、5.8s/0.63GiB、9.1s/0.88GiB vs 0.32 的 8.95/0.88、12.08/1.35、22.30/2.33）。
- 几何遍结果以定容 `Snapshot` 按值返回，填表阶段不再重新展开整个布局（单遍）；容量溢出自动回退两遍。
- 新增 `REGFW_REQUIRE_NONOVERLAP_FAST_PATH` 严格模式：无法走快路径时按 ActualOverlap/ReservedIntersection/SelfOverlap/OutOfBounds/IdentityBoundary/ArithmeticOverflow/UnsupportedGeometry 分类终止编译；`StaticBuild::stats` 提供挂载数/元素数/word 数/页数/重定向数/路径与首个回退原因。
- 新增 `benchmarks/poe_layout.cpp`（约 2000 声明、8 VF、PF-shared 17 处重挂载、混合宽度、稀疏数组、双 ACC 地址族）：快路径 39.1s/3.39GiB vs 强制完整构建器 52.7s/4.68GiB（268M ops 上限内均可完成）。
- 测试：宽度混合、非单调多维（512-bit dims(2,3) strides(0x180,0x100)）、2048-bit 稀疏、互补 View 叠加与真实覆盖回退、宽寄存器 identity 别名、stats 断言全部纳入 static_regular 差分电池；static_builder 改用元素挂载 API；严格模式纳入编译失败契约（GCC 13/14 与 Clang 18）。

## 0.33.2

- 文档修正：Performance.md 的轻量入口误用诊断描述更新为 `static_assert(REGFW_COMPILED_LAYOUT_ENTRY)`（原描述为 0.33.0 的 DeclaredLayoutRoot 诊断）；安装消费者版本引用更新为当前版。
- 文档修正：LayoutCompilation.md 推荐写法改为声明头包含 `<regfw/bundle.hpp>`、布局 .cpp 包含完整 `<regfw/regfw.hpp>`——避免使用者在声明头继续引入布局编译器，使轻量入口真正减少普通消费 TU 的依赖；完整入口在声明头中仍受支持。

## 0.33.1

- 轻量入口误用拦截改为编译期 `static_assert(REGFW_COMPILED_LAYOUT_ENTRY)`：仅含 `<regfw/bundle.hpp>` 的 TU 使用 `REG_LAYOUT`/`REG_LAYOUT_IMPL` 时，GCC 13/14 与 Clang 一致地立即报错。0.33.0 依赖 DeclaredLayoutRoot 局部类型缺定义的诊断，GCC 14/Clang 下 `-fsyntax-only` 会放行、错误推迟到链接。
- `benchmarks/measure_headers.py` 峰值 RSS 改为 `os.wait4` 按子进程统计（原先 `getrusage(RUSAGE_CHILDREN)` 是历史子进程累计最大值，不可用于逐项对比）。
- `tests/static_regular.cpp` 发射回归加强：对发射后的 word-hash 与 page-hash 两种索引，全部映射地址探测必须命中正确 word，随机未映射地址必须在一次表扫描内有界终止并返回未命中。
- 补充 GCC 14.2 全量回归（Release 31/31）与 Clang 18 编译失败契约（12/12）；Clang 全量测试受基线测试代码的 clang 专属告警阻塞，为既有边界。
- `parallel.py --jobs 24` 在 7.65 GiB 容器通过（含运行检查）；`-j32` 需约 8 GiB 以上内存（0.32.0 发布记录峰值 8.00 GiB），本环境不可用，待目标服务器验收。

## 0.33.0


- 规则布局直接填写最终静态计划：消除候选表、归并排序、临时 words/redirects/pages 缓冲与最终复制；复杂覆盖布局保留完整构建器，两路径计划语义逐字段差分一致。
- 寄存器声明形状（计数/占用/密集证明/维度排序）按声明类型求值一次并跨挂载共享；稠密转置数组的物理→线性序表按声明共享且不进入二进制。
- 新增轻量声明入口 `regfw/bundle.hpp`（不含 constexpr 布局编译器）；`declarations.hpp`/`regfw.hpp` 兼容行为不变；轻量入口使用编译版 REG_LAYOUT 产生明确编译错误并纳入测试。
- `regfw_precompile_bundle` PCH 辅助获得端到端测试覆盖（bundle_pch），新增 `benchmarks/measure_headers.py` 覆盖 full/light × PCH × 并行构建矩阵。
- 修复 GCC 13 consteval 静态对象发射中查找索引哨兵槽清零导致的探测死循环（0.32.0 及更早存在）：显式赋值全部索引槽位并增加运行期发射检查。

## 0.32.0


- 访问日志等级判断前移，避免为不输出的映射访问日志准备完整上下文。
- 规则布局按数组块计数，复杂覆盖继续完整编译期解析。
- 初始化直接绑定重定向索引，复用 scope 优先级。
- 清理历史测量材料并更新当前使用说明。

## 0.31.0

- 新增可分离的 REG_LAYOUT_DECL / REG_LAYOUT_IMPL，静态或动态布局定义可放在单个 .cpp。
- 编译期临时缓冲、自然归并、轻量计数、容量预估和身份重定向填充优化；保持最终分发表和全部运行期实现。
- 增加跨翻译单元、计数/完整构建对照、覆盖重定向及并行构建验证。


## 0.30.0

- RegScope 声明冲突检查由累计线性扫描改为有序区间查询，保留空洞、对齐和重叠校验。
- 数组内部按元素占用区间检查冲突，消除每个 32 位片段的临时哈希项和重复名称副本，支持非单调多维 strides。
- 静态映射复用每组名称前缀，预留元素地址容量。
- 增加初始化专项回归及大数组/9 实体/DAG 双入口分阶段对照。公开写法、QEMU 接入和快照 v2 不变。

## 0.29.0

- 普通与映射 scalar 路径按读/写分别编译，去掉入口已验证的重复静态条件；动态条件保留。
- 分发表分别选择读写处理类别，跳过逐条试探普通/映射快路径；热点记录仍为 32 字节。
- 增加 resolver 生命周期/异常/析构重绑回归，保留动态钩子、日志、事务及快照 v2。
- 使用端接口及 QEMU 入口保持不变；内部布局变化要求头文件和库配套重编译。


## 0.28.0

- 自动收集 REG / GROUP / ENTITY 静态描述；REG_SCHEMA 成为兼容用可选项。
- 同一 REG_LAYOUT 宏新增无捕获符号工厂 [](auto layout)，支持 layout.vf[i] 和嵌套实体，无需成员指针。
- 提供 layout.view/address_space 与 reg::compiled 的原名构造器；保留动态 AddressView 返回类型及循环赋回行为。
- 复用已有 consteval 计划和 MMIO 路径；增加自动 schema、跨翻译单元、完整 POE 示例和错误编译验证。


## 0.27.0

- 新增 REG_STATIC_LAYOUT / static_view / static_address_space / target：固定布局在 consteval 中生成地址选择、权限、身份族、覆盖诊断、执行路径类别、重定向目标和只读地址索引。初始化仅绑定实际 Entity、命名 resolver/pre_access 槽及诊断名称。原 REG_LAYOUT 继续支持运行期布局。
- REG_SCHEMA 显式列出实体成员指针，复用已有 REG 规格，不重复填写偏移和宽度；不使用跨翻译单元不稳定的隐藏注册计数器。遗漏成员在绑定时拒绝，重复成员和非法静态布局在编译期拒绝。
- 新增 REG_STATIC_INDEX，编译期确定常见单 uint32 坐标映射的维度、范围和线性步长；动态 resolver 每拍调用，复杂组规则保留原入口。
- 分发表绑定直接/身份解析函数；减少大映射记录复制，身份结果直接选择预先绑定的目标，单坐标映射减少通用维度处理。所有动态绑定、权限、回调/异常、pre_access、事务和快照语义保留。
- 初始化默认遵循项目优化等级；需要旧的编译速度策略时显式定义 REGFW_FAST_COMPILE_INIT。
- 性能报告同时比较 0.26.0、新动态布局、新静态布局的初始化阶段及 MMIO，对照 switch，保留全部样本和退步项。

## 0.26.0

- 初始化完成时生成紧凑的标量分发记录，直接保存存储地址、掩码和静态访问方向；动态回调、pre_access 和日志等级继续逐拍检查。
- 冻结稀疏页目录为低装载率的连续探测表，访问不再查 unordered_map；不按高地址之间的空洞分配存储。
- 将实体的回调及标量访问状态集中放置，减少访问冷元数据；下标按字段复制，避免两个小结构的 rep movsl 开销。
- 原生 32-bit 回调采用轻量调用帧，在与用户函数一起编译的适配器中按需准备完整 info；读回调两条路径共享同一可变函数对象。
- 常见单坐标、无条件的命名 index 映射直接准备字段，继续调用当前 resolver 并保留下标范围/冲突验证。
- 新增高地址页冲突、空洞、权限、晚绑定和恢复回归。公开建模 API 与快照 v2 保持，头文件和库需配套重建。

## 0.25.1

- finish 时预编译原生 32-bit RW 的静态分发条件，逐拍保留动态回调、pre_access、logger 和借用状态检查。
- VF/index 解析和验证后可直接访问符合条件的原生标量，动态 resolver 每次照常运行，不缓存路由结果；无业务回调时跳过后续事件函数和上下文更新。
- 复用首次地址查询及每 Map 的解析/回调上下文，减少大记录初始化和中间复制；每次清除身份、宽值指针及派生字段等临时状态。
- 命名 index resolver 的稳定槽直接保存在访问计划中，减少函数包装层，保留换绑/清除及回调自修改的生命周期。
- 扩展快慢路径差分，覆盖双高地址、动态 VF/index、全部 16 字段、读写/读完成回调、快照和 resolver 自换绑。

## 0.25.0

- 为满足条件的 AddressView 32-bit 普通 RW 增加快路径；每次访问检查权限、宽度、回调、钩子和事务条件，其他情况继续完整分发。
- 每个 Map 复用派生字段暂存区，按值复制独立持有有效字段；保留 16 字段容量与缺失字段检查。
- 缓存原生标量存储地址，减少虚函数访问和无变化写回；自定义值编码保持原有存储语义。
- 增加快路径差分与动态绑定回归，保留映射、行为、回调、日志、复位和快照契约。

## 0.24.1

- 文档补充 on_read 返回 nullopt/void 使用回调前候选值、不重新读取已修改 entity 的注意事项及示例。
- View/global pre_access 热路径取消每次调用的 shared_ptr 复制，通过 pre_access 链执行期间重绑/清除时暂存旧函数来保证生命周期安全。
- 旧函数在本次 pre_access 链结束前释放；保留回调内自重绑、自清除和异常语义。
- 钩子链的 View 名称改为借用稳定描述，减小链记录；新增自修改回调生命周期测试与复测。

## 0.24.0

- AddressView 支持 pre_access(fn) 和 using_pre_access(key)，新增 REG_PRE_ACCESS 声明以及 map.set/clear/has/defer_pre_access(key) 命名绑定。
- 构建时预编译最终选中 View 的父子钩子链，MMIO 不扫描 View 树；identity 重映射后使用目标实例路径。
- 保留全局钩子，执行顺序为全局、父 View、子 View，每个有效 MMIO beat 执行。覆盖失败不回退，异常沿用既有 pre_access 语义。
- 新增 view_path 元数据、静态钩子诊断和完整行为/性能验证。

## 0.23.3

- 修正布局表把每个实体映射入口标为 ENTITY 的歧义，统一显示 VIEW → entity: 原始存储目标名。
- 保留实体原始组名，PF/PF_VF/VF 入口显示同一共享目标；reset 仍表示存储复位归属。
- 明确折叠重复族使用代表实例，以及地址范围为映射边界而非实际 REG 覆盖范围。

## 0.23.2

- AddressView 新增 map_repeat(name,offset,repeat,factory,priority)，支持局部组合完整模块后再挂载多个高地址入口。
- 保留重复族身份解析、局部优先级、空洞与边界检查，不复制实体或事务状态。
- PF/VF 公开示例改用大 View 组合，新增 32 MiB 双入口、VF 重映射、共享 PF 实体和跨入口快照回归。

## 0.23.1

- 修复紧凑寄存器地址表按名称排序的问题，改为按每行最小可见入口地址进行 uint64_t 数值排序，保留数组/重复族折叠。
- 配置 CMP0144 NEW，消除源码集成及安装包 find_dependency(HLog) 使用 HLOG_ROOT 时的兼容性警告。
- 新增高地址双入口、逆序命名和数组首元素被覆盖时的排序回归。

## 0.23.0

- 删除旧 Window 公共 DSL 及严格窗口编译路径，只保留 Entity / AddressView / AddressSpace 布局。
- Debug 布局仅展示 View/Entity；寄存器地址细节真正放在 Trace。
- Trace 数组按维度/stride/地址公式浓缩；重复实例只列代表，部分覆盖显示可见选择。
- 示例、Agent 契约、迁移说明和布局测试统一使用新 API。


## 0.22.0

- 新增可复用 AddressView / AddressSpace：用 view(name,size).map(target,offset,priority) 组合实体或子视图，address_space.map/map_repeat 挂载多个入口。
- 64 位地址及稀疏索引，支持远距高地址入口；身份请求地址和实体布局地址不再截断到 32 位。
- View 空洞透明，优先级在兄弟间比较；实际实体遮挡生成 REGFW_ENTITY_SHADOWED warning，同优先级实体冲突报错。
- 映射在构建时编译到实体直接分发表，运行时不遍历优先级；实体存储、回调、reset、事务及快照保持独立。
- 旧 Window 布局保留原有严格检查；新实体/View 组合需从无地址实体构建，不与旧式有地址存储混用。

## 0.21.0

- 删除 CommandPort、command window、REG_COMMAND_REGION 与独立命令解码；命令全部声明为 REG 实体，使用统一回调、事务、复位和快照。
- 重写访问日志：Debug 和 Trace 均为单行，Trace 使用 ` | ` 分隔详细字段；方向名称、多维下标、仅变化映射、合并地址链，ERROR 保留完整路径，去掉完整 C++ 签名前缀。

- 删除 Split64Policy、split64 builder 和专用锁存/暂存状态；所有位宽统一 TransactionPolicy。64 bit 默认仍独立，需要事务时显式配置，原完整 8 字节限制改为 U64_ONLY。

- 完整快照格式 v2：保存 Migratable 实体值及每元素的 统一 transaction 写 pending/mask、读快照和成功标记。
- RegisterBank 增加 save()/load(snapshot)、Writer/Reader 和可选 void* 流重载，支持原有 VMSTATE_FIELDS(bank, ...)；沿用 hac::snapshot/module_var_save/load，不引入 VMStateDescription。
- 恢复先验证完整 schema/形状/策略/状态和 payload，再统一提交；错误不改变 bank，恢复不重放回调且保持原生值引用有效。
- v1 版本化快照明确拒绝；旧裸 word span 接口仍为值数组操作。
- 新增分拍续传、损坏流、父对象字段递归测试，以及完整 Agent 文档和快照接入说明。

## 0.20.1

- 公开示例收敛为 quickstart 和 PF/VF 完整映射，统一使用无地址实体 + 递归 window。
- 删除过时布局案例、旧教程和旧快照；有效的读回调/下标检查作为独立测试保留，布局测试使用当前 PF/VF 示例。
- 重写 README 和专题文档；示例代码从编译源文件提取，布局/解释输出从可执行程序生成。
- CTest 检查文档生成内容、SDK 内 Markdown 文件链接和实际输出；发布检查独立示例构建，避免测试程序混入公开示例。
- 发布包清理测试日志等产物，随包配套 hlog 3.2.1；寄存器框架运行时代码未改动。

## 0.20.0

增加带声明定位的布局报错、统一 RO / identity Ignore 语义、MMIO 截断和宽寄存器首 beat 诊断、布局警告及静态 explain_address。引入解包、CTest、安装消费者、ASan/UBSan 的发布验证。

## 早期 API

0.19.0 接入 hlog 原生日志。后续加入 RegEntity/RegView 与递归 window 布局；旧 aperture/alias 接口保留兼容。当前迁移路径见 [Migration](docs/Migration.md)。
