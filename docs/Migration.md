# 0.36.0 的内部建表迁移

正式生产后端是原生单遍布局生成 + 共享 EntityLayout Hash。公开 API、DSL、MMIO、Snapshot 均不变；依赖 `reg::detail::compile_formula_layout` 的私有代码必须迁移到 `reg::detail::build_formula_layout_runtime`，但设备业务源码不应依赖这些私有符号。几何布局错误从编译期诊断迁为初始化期配置错误。

# 0.35.0：删除旧 word 后端

- `REGFW_LEGACY_STATIC_WORDS` 已删除；定义它不再有任何效果。
- `StaticWord`/`StaticShadow`/`StaticPlan`/`StaticPlanView`/`compile_static_layout`/
  `install_static_layout` 是内部类型/函数，不在公开 API 中；如业务代码直接引用，需改为
  `build_formula_layout_runtime`/`install_formula_layout`（仅供内部使用；正常 REG/REG_LAYOUT 代码无需改动）。
- reserve 语义正式为半开 `[base, base+size)`；依赖旧的“size 当绝对 end”写法的布局需要
  修正。溢出会得到 `formula layout: ReserveRangeOverflow`。
- 真实寄存器字节重叠现在按绝对地址诊断；包围盒重叠但字节不重叠的 View 合法。
- 快照 schema/hash、Reset、QEMU MMIO 地址语义不变。
- 动态 `AddressView` 也由 `FormulaDecoder` 安装；对外 API 不变，但不再有逐 word
  `compiled_views_` 索引。动态布局元素很多时，mount 数按元素增长；如遇构建期
  `UnsupportedGeometry` 或内存峰值升高，请提供布局形状以便评估分组策略。
- 旧 builder 内部测试现移至单独的历史测试归档，不参与构建；可观察解码由
  `tests/formula_golden.cpp`（1321 条冻结 word）和 dynamic-vs-formula 差分覆盖。

# 0.34.0：Formula Decoder

- 公开 API、REG/布局 DSL、CMake target 和 MMIO 调用方式不变；业务源码无需修改，重新编译即可。
- 默认使用公式后端；如需逐 word 旧后端，定义 `REGFW_LEGACY_STATIC_WORDS`。旧后端保留用于历史冲突布局和差分对照，不会因新后端默认启用而删除。
- 构建期若出现 `formula layout: UnsupportedGeometry` / `SelfOverlap` / `IdentityBoundary` / 地址冲突：先检查布局是否真的重叠或元素是否跨 identity 边界；不要通过放宽检查绕过。需要兼容旧行为时用旧后端，并把该布局作为差分用例。
- 不再需要任何额外初始化调用；`REG_LAYOUT_DECL`/`REG_LAYOUT_IMPL` 分离 TU 写法不变。
- 快照 schema/hash 和 reset 语义未变，0.33.3 生成的快照可继续加载。

# 0.33.3：稀疏规则构建器

本次优化不要求修改声明、layout、回调或 QEMU 接口。可选：只声明寄存器的设备头把 `#include <regfw/regfw.hpp>` 换成 `#include <regfw/bundle.hpp>`，并在定义 REG_LAYOUT_IMPL/REG_STATIC_LAYOUT 的 .cpp 保留完整入口；不换也完全兼容。仅包含 bundle.hpp 的 TU 不能使用带工厂的 REG_LAYOUT（会得到明确编译错误）。

## 0.32.0

本次优化不要求修改声明、layout、回调或 QEMU 接口。请配套更新头文件和库并重编译；快照格式保持 v2。测量及适用边界见 [Performance](Performance.md)。

0.31.0 支持布局定义移到单个 .cpp：头文件 `REG_LAYOUT_DECL();`，实现文件 `REG_LAYOUT_IMPL(Type, [](auto layout){ 原布局函数体 });`。构造器可配合 REG_BUNDLE_DECL/IMPL。详细约束和完整示例见 [布局编译与独立 .cpp](LayoutCompilation.md)。旧 REG_LAYOUT 保持兼容。

> 0.30.0：初始化优化不要求迁移声明或布局，头文件与库配套重编译。校验、诊断和快照语义保留；见 [初始化性能与边界](Performance.md)。

> 0.29.0：本次使用端 API 不变，头文件与库需配套重编译。resolver 结果仍逐次计算，支持调用内换绑/清除，旧对象在该调用作用域退出时释放。日志/钩子/事务/快照行为保持，QEMU 入口未改。详情见 [性能与验证](Performance.md)。

# 0.28.0 前端简化

固定布局改用 `REG_LAYOUT([](auto layout){...})`；去掉 REG_SCHEMA，target 成员指针改为 layout.member / layout.vf[i]。用 layout.view / layout.address_space 或 lambda 内的 reg::compiled 命名空间别名。见 [完整迁移说明](StaticLayout.md)。旧动态 API 无需迁移，旧静态 API 仍可继续编译。头文件和库配套更新；回调 API v4、快照 v2 不变。

# 从旧 API 迁移

[返回 README](../README.md)

0.20.1 整理使用入口，保留旧 API 的运行时兼容性。新项目只需阅读两个 [公开示例](../README.md#构建并运行)。过时的公开示例与教程已删除；当前功能由测试和两个公开示例验证。

| 旧形式 | 当前推荐形式 |
|---|---|
| `REG_APERTURE` / `REG_REPEAT_APERTURE` 同时表达存储与地址 | `REG_ENTITY` / `REG_REPEAT_ENTITY` 创建存储；View / AddressSpace 描述地址 |
| 带 aperture 参数的 `REG_ENTITY` | 无 aperture 的实体声明；reset 配置写在 `reg::entity()` 上 |
| `REG_VIEW` / `REG_REPEAT_VIEW` 声明空地址窗口 | `reg::view(name,size).map(...)` |
| `.aliases` / `.views` / `.identities` 并列表 | `REG_LAYOUT([&] { return AddressSpace; })` |
| `reg::alias(source).to(target)` | 在 View 上 `.map(target)` |
| `.to(vf)` 隐式重复配对 | repeat factory 中明确 `.map(vf[i])` |
| 全局身份 resolver | View 上 `.identity(domain).using_resolver(key)`，初始化时按 key 绑定 |
| 独立的 reg::Logger / sink | hlog 原生 logger |

旧 API 仍可用于维护已有设备；不要在同一推荐示例里混写两套布局。更新本版须同时更新头文件与 src 并重新编译。CMake 已维护完整源文件清单，优先链接 `regfw::regfw`。

从旧示例路径迁移运行入口：

| 原路径 / 目标 | 当前入口 |
|---|---|
| `examples/full_mapping*`、`poe_layout*` / `regfw_full_mapping` | [pf_vf_layout.hpp](../examples/pf_vf_layout.hpp) + `regfw_pf_vf` |
| `examples/address_explain.cpp` | `regfw_pf_vf --explain` |
| `examples/POE_LAYOUT.txt` | [实际 PF/VF 布局输出](output/pf-vf-layout.txt) |
| `examples/interleaved*` | 学习嵌套 repeat 看 [AddressViews](AddressViews.md) |
| `examples/unified_read.cpp` / `index_mapping.cpp` | 使用说明见 [设备逻辑](Device.md)；有效的读回调/下标测试保留在 tests 中 |

0.20.0 已修正的语义仍保持：寄存器 RO 写返回 Ignored 并跳过写回调；identity Ignore 的读写都返回 Ignored；默认 MMIO 仍是 4 字节，8 字节必须明确传 size。依赖旧 RO 回调副作用的代码应按 [诊断说明](Diagnostics.md) 调整。

## 0.21.0：统一 transaction

旧 Split64Policy 和 `.split64(...)` 已删除，不保留专用运行时状态。迁移如下：

| 原需求 | 现在的配置 |
|---|---|
| 独立访问 | 不配置，或 TransactionPolicy::INDEPENDENT |
| 低端首读快照、高端完成；高端写提交 | TransactionPolicy::LOW_TO_HIGH |
| 高端首读快照、低端完成；低端写提交 | TransactionPolicy::HIGH_TO_LOW |
| 只改变读或只改变写 | 显式 TransactionPolicy{SplitReadMode,SplitWriteMode}，另一侧 Independent |
| 只允许完整 64 bit 访问 | access_widths(RegisterAccessWidthPolicy::U64_ONLY) |

旧 latch 在后续拍还会取一次读源；新的事务快照只在首拍调用读源。需要该旧副作用的设备应审查回调，不要把 latch 名称直接当成 transaction 的等义别名。

完整快照与旧 module_var_* 字段流接入见 [Snapshot](Snapshot.md)。

## 0.21.0：命令统一实体

删除 CommandPort、window.command、map.commands/create_commands、REG_COMMAND_REGION。声明 CommandRegs : RegEntityBundle，在其中用 REG 定义 doorbell 或完整宽度命令；根布局用 REG_ENTITY/REG_REPEAT_ENTITY 创建存储，原命令窗口改为 map 对应实体组。

回调绑定到 bank->pf_commands.DOORBELL 或 bank->vf_commands[i].DOORBELL 等普通成员。无回调不再返回旧命令区的 AccessError，而是执行实体 behavior；未声明位置 DecodeError。512 bit 命令用一个宽 REG 和 transaction，不用 16 个独立 uint32。


## 0.23.0：高地址与可组合 View

头文件和编译库一起升级。IdentityResolveRequest 的 bus_offset/canonical_offset/offset_in_instance 和布局地址已扩为 uint64_t；按值拷贝此结构的外部代码必须重新编译。VF ID 与下标类型未变。快照仍为 v2，不保存分发表；目标必须先重建布局。

旧 Window DSL 已删除，reg::Window、reg::window、sub_windows 及 window.hpp 不再是公共 API。若要优先级叠加，将根 REG_LAYOUT 改为返回 address_space；实体仍用 REG_ENTITY 声明，内部组合用 view(name,size).map(...)。不要在新根中使用有地址的 REG_APERTURE 存储，也不要再调用旧 apply_layout 追加窗口。完整迁移片段见 [AddressViews](AddressViews.md)。

新的 space() 不传大小时使用默认 64 位范围；原来 space(bytes) 可继续限制上界。两个巨大 base 之间的空洞不会创建寄存器或分配页目录。

## 0.23.0：只保留 Entity / View 布局与分级日志

原 window 树必须迁移，头文件与静态库一起升级；原实体和回调声明可保留。

| 删除的布局写法 | 当前写法 |
|---|---|
| window(name).size(S).map(entity) | view(name,S).map(entity) |
| child.offset(O) | parent.map(child,O) |
| root.sub_windows({a,b}) | view(name,S).map(a,OA).map(b,OB) |
| 根 window().size(S) | address_space(name).map(view,BASE)；上界由 bank 的 space(S) 约束 |
| sub_windows(repeat(...),factory) | AddressView.map_repeat(name,offset,repeat(...),factory) 或 AddressSpace.map_repeat(name,base,repeat(...),factory) |
| reserved window | view.reserve(offset,size,priority) |

Debug 不再包含逐 REG 的地址表；需要寄存器布局时启用 Trace，或显式调用 describe_layout(LayoutDetail::Full)。多维数组、宽寄存器和重复 VF 使用紧凑公式，不逐地址展开。

0.23.2 新增 AddressView.map_repeat。0.23.1 及以前可在 AddressSpace 根重复挂载，不能直接调用 View.map_repeat；本版可把原来的根组合搬到 view(name,size)，再让根只 map 大 View。

## 0.24.0：View pre_access

原全局 map.set_pre_access(fn) 保持；按入口筛选的检查可改为 REG_PRE_ACCESS 声明、View.using_pre_access(key)、初始化时 map.set_pre_access(key,fn)。全局函数若保留也会先执行，避免不小心重复检查。RegAccessInfo 新增借用的 view_path 字段，头文件与编译库需同步升级；快照仍为 v2。

0.24.1 优化了钩子调用：普通 MMIO 调用不复制函数的 shared_ptr；仅当回调在 MMIO 中重绑/清除时，保留旧函数至本次 pre_access 链结束。回调内自修改仍受支持，后续钩子读取最新绑定；旧捕获对象的析构时点可能延后到 MMIO 返回前。框架仍不支持未同步的并发重绑。

## 升级到 0.26.0

公开建模 API 和访问语义保持不变，不需要改写 Entity/View 或回调。重新构建整个设备工程，配套使用本版头文件与 libregfw.a；不要混用不同版本的内部访问计划和回调适配器；从 0.24.x 升级时 RegAccessInfo 的二进制布局也发生变化。不要对 RegAccessInfo 做 memcpy 或裸字节序列化；通过正常值复制保存所需字段。快照 v2 及银行 save/load 接口不变。详见 [MMIO 优化](Performance.md)。

## 0.26.0：内部执行计划与轻量回调帧

公开声明、绑定方式和回调签名保持，配套替换头文件和编译库并重新编译设备工程。内部 EntityState、地址记录和标量读回调适配器已改变，不能混用二进制。正常 info 值复制仍支持，借用期和外部同步要求不变；同一可变读源在分拍/完整路径之间保持一份函数状态。快照格式仍为 v2，重建相同布局/回调后恢复，执行计划和函数状态本身不保存。


## 0.27.0：可选择静态布局，动态入口保持

配套替换头文件和库并重编译；内部 ABI 改变，快照格式仍 v2。原 REG_LAYOUT / REG_ACCESS_GROUP 不要求迁移。固定布局迁移使用 REG_SCHEMA 成员指针列表和 REG_STATIC_LAYOUT 无捕获工厂；不能简单把原来捕获 this 的 lambda 改名。精确示例见 [StaticLayout](StaticLayout.md)。复杂动态布局可以继续原入口，不以删除能力换性能。

初始化默认遵循项目优化等级。原强制 O0 的编译时间策略改为显式 REGFW_FAST_COMPILE_INIT。新增静态布局增加常量求值与只读表成本；GCC 大布局可能需要提高 constexpr 运算限额，参见静态布局文档。
