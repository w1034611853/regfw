# 同步分支架构精简

2026-11-22。实现基线为 `b1059f5`，性能基线为同一生产代码的 `9f9f4a2`。
生产源码统计只计 `src/` 和 `include/`，不计测试、报告和基准。

| 阶段 | 新增 | 删除 | 净变化 |
|---|---:|---:|---:|
| 交错布局统一使用 Table | 41 | 384 | −343 |
| 单轨地址分发 | 383 | 2704 | -2321 |

## 交错数组

非单调形状使用现有按物理偏移排序的 Table，保留声明顺序的线性坐标。
静态工厂每个声明最多 16,384 条，动态 AddressSpace 每个映射段最多 1,048,576 条。
仓库现有合法交错用例均在上限内，保留原上限。超限在配置阶段失败。排序后比较完整占用区间，
会拒绝起始地址不同但存在字节重叠的声明。

`formula_fastpaths` 断言 rank 2 和 rank 3 静态交错数组走 Table，同时对静态和动态布局验证
坐标、空洞及 MMIO。原 `formula_geometry`、`formula_diff`、`formula_golden` 和
`formula_regressions` 行为对照均通过。GCC 14.2 严格 Release 通过 35 项 CTest。


## 单轨地址分发

平坦 RegisterBank 用同一套 ViewCompiler 安装默认根 View，允许在绑定 finish 前访问，
纯 storage-only bank 可以之后显式安装 AddressSpace。直接 RegisterMap::create_reg
调用先声明和绑定，再 finalize 安装默认 View。直接 API 跨实体的地址冲突在安装默认
View 时验证。默认 View 的 1/2/4/8 字节访问与显式 View 使用同一套解码与限制规则。
配置完成的初始布局不依赖访问历史，MMIO 未添加逐次状态或缓存。

删除的手动 API 包括 RegisterMap 的 `map_alias_window`、
`map_alias_window_with_policy`、`map_routed_alias_window`、
`map_routed_alias_window_with_policy`、`reserve_region` 及其内部 source-instance、
identity repeat window 和 view policy 接口。同步删除 `RegLayout`、`RegScope::alias/reserve`、
旧 `LayoutSpec` 的 `.views/.aliases/.identities/.reserved` 列表及
`RegViewSpec/AliasSpec/IdentitySpec/ReservedSpec` 对应构造器。LayoutSpec 的 `.root` 和
动态、符号及独立翻译单元 REG_LAYOUT 继续使用 AddressSpace。
布局报告用同一张可见寄存器表说明实际映射。

| CTest | 迁移或移除内容 |
|---|---|
| entity_view | 重复入口、共享实体、命令和只读入口改为 AddressSpace，仍验证 V2 快照、跨 bank 和失效引用 |
| address_view | 64 位地址上界用默认根 View，保留其他用例中的双入口和重定向 |
| diagnostics | RO 权限、identity、宽寄存器、解释地址换用显式 View。解释展示选中 View |
| layout_report | 重复保留区、局部视图迁移。删除旧报表的链式/排列 alias 表和 RegLayout 专用断言 |
| resolver_safety | 使用命名身份窗口验证未绑定、越界、抛错、Ignore 与 QEMU 错误位 |
| resolver_lifetime、read_callbacks | 低级 RegisterMap 用例完成声明及绑定后 finalize |
| init_validation | 删除 SparseLookup 专用测试，保留随机 claim、空洞、重叠与快照对照 |
| view_validation | 保留纯 storage-only bank 的延后布局安装测试 |

GCC 14.2 严格 Release 通过 35 项 CTest。`mmio_scalar` 的 alias 模式使用两个
AddressSpace 入口，配对基线与候选使用相同的基准源码。
