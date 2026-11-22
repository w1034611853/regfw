# 同步分支架构精简

2026-11-22。实现基线为 `b1059f5`，性能基线为同一生产代码的 `9f9f4a2`。
生产源码统计只计 `src/` 和 `include/`，不计测试、报告和基准。

| 阶段 | 新增 | 删除 | 净变化 |
|---|---:|---:|---:|
| 交错布局统一使用 Table | 41 | 384 | -343 |
| 单轨地址分发 | 383 | 2704 | -2321 |
| MMIO 错误返回 | 631 | 451 | 180 |
| 相对 b1059f5 总计 | 1029 | 3513 | -2484 |

## 交错数组

非单调形状使用现有按物理偏移排序的 Table，保留声明顺序的线性坐标。
静态工厂每个声明最多 16,384 条，动态 AddressSpace 每个映射段最多 1,048,576 条。
仓库现有合法交错用例均在上限内，保留原上限。超限在配置阶段失败。排序后比较完整占用区间，
会拒绝起始地址不同但存在字节重叠的声明。
原先可以用数论公式解码的大型交错声明，现在需要排序并保留每个元素的表项。
构建时间和表空间随元素数量增长，超过原有上限的交错声明会收到配置错误。

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
`map_routed_alias_window_with_policy` 和 `reserve_region`。同步删除
`register_source_instance`、`register_identity_repeat_window`、`view_policy`、
`RegLayout`、`RegScope::alias` 和 `RegScope::reserve`，
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


## MMIO 错误返回

解码、宽度/对齐检查、段越界、身份和下标校验用内部失败值传递结果。失败值含
MemTxResult、ErrorKind 和诊断。对齐 32 位地址未命中在首次查找后返回。配置阶段和
公开抛异常的地址查询接口仍使用 Error。核心读写入口的外层 try/catch 已移除，外部
resolver、pre_access、读写/完成回调及设备提供的 behavior/codec 保留调用边界。
C ABI 与 logger 仍有 noexcept 边界保护。回调造成的异常链与指定错误类别占用仅失败
路径传播，Identity + Ignore 的映射沿用原规则。读的 MAGIC + Ok、写钩子的 Ignored、
事务、快照及按需 info 的回调入口保留。

GCC 14.2 严格 Release 全部 35 项 CTest 通过。resolver_safety 同时覆盖命名 resolver
返回负值、返回越界 ID 和主动抛 Identity 错误，以及这些错误在 Ignore 策略下的转换。

## 错误探测配对

Xeon Platinum 8573C，CPU 1，GCC 14.2，Release，无 LTO。每个请求类型 5 轮交替配对，
每个进程 40 万次。logger 关闭，基准内校验状态码、无效读值和存储。
列出两个独立配对组的中位数（ns/次），以隔离阶段 3 的改动。

| 请求 | 9f9f4a2 | 最终 | 29512da（单轨） | 最终（同轮） |
|---|---:|---:|---:|---:|
| View 内空洞读 | 1368.90 | 10.21 | 1217.28 | 10.42 |
| View 内空洞写 | 1349.22 | 8.70 | 1229.71 | 8.81 |
| 地址超出 map 读 | 1207.52 | 9.38 | 1320.02 | 9.70 |
| 地址超出 map 写 | 1245.58 | 7.84 | 1323.65 | 7.75 |
| 地址未对齐读 | 1249.15 | 9.21 | 1300.06 | 8.91 |
| 地址未对齐写 | 1228.94 | 8.11 | 1310.56 | 8.16 |
| 非法宽度读 | 1236.14 | 7.52 | 1284.79 | 7.18 |
| 非法宽度写 | 1228.26 | 7.56 | 1328.44 | 7.43 |

单轨版本的同轮对照约快 117～179 倍。诊断实际输出时另有日志格式化和 I/O 开销。


## 正常访问配对

`measure_mmio_scalar.py --quick` 测 256 个元素、完成绑定、logger 关闭的 24 个场景，
每个进程 500 万次，6 轮配对。以下为代表值，单位 ns/次。

| 请求 | 9f9f4a2 | 最终 | 变化 |
|---|---:|---:|---:|
| 平坦，无回调读 | 7.78 | 9.17 | +17.8% |
| 平坦，无回调写 | 5.51 | 7.24 | +31.5% |
| 平坦，轻量回调读 | 11.02 | 12.11 | +9.9% |
| 平坦，轻量回调写 | 7.93 | 10.31 | +29.9% |
| 平坦，完整 info 读 | 12.12 | 13.17 | +8.7% |
| 平坦，完整 info 写 | 10.65 | 11.78 | +10.7% |
| 显式单 View，无回调读 | 11.90 | 9.67 | -18.7% |
| 显式单 View，无回调写 | 9.07 | 7.46 | -17.8% |
| 显式单 View，轻量回调读 | 13.05 | 12.08 | -7.4% |
| 显式单 View，轻量回调写 | 11.36 | 10.17 | -10.5% |
| 显式单 View，完整 info 读 | 14.99 | 12.48 | -16.7% |
| 显式单 View，完整 info 写 | 12.72 | 11.81 | -7.2% |
| 双入口，无回调读 | 12.80 | 12.32 | -3.7% |
| 双入口，无回调写 | 10.67 | 9.17 | -14.1% |
| 双入口，轻量回调读 | 14.19 | 14.02 | -1.2% |
| 双入口，轻量回调写 | 11.81 | 11.95 | +1.2% |
| 双入口，完整 info 读 | 16.01 | 16.09 | +0.5% |
| 双入口，完整 info 写 | 12.73 | 12.75 | +0.1% |

单个 View 时，解码器直接检查该 View 的范围再查共享字表。隔离对照中，这一步将
初版单轨的平坦无回调读取从 14.83 降到 10.05ns。相比本轮基线的直连页索引，平坦布局仍
多了 View 选择和绑定定位，最终无回调读写分别多 1.39 和 1.73ns。无参回调仍绕过 info，
完整字段仍在调用适配器内按需展开。显式单 View 的常见标量访问在本轮对照中受益，
双入口已按当前 AddressSpace 写法对照。

`measure_layout_study.py` 运行 4 轮多 View、Reserve、identity + index、内存与初始化对照。
映射使用完整 info resolver。以下为访问中位数（ns/次）。

| 请求 | 9f9f4a2 | 最终 | 变化 |
|---|---:|---:|---:|
| 64 View 稠密读 | 18.10 | 17.98 | -0.6% |
| 64 View 稀疏读 | 21.91 | 22.36 | +2.0% |
| 64 View 局部 Reserve 写 | 21.05 | 21.06 | +0.0% |
| identity + index 无回调读 | 69.21 | 69.56 | +0.5% |
| identity + index 无回调写 | 65.67 | 69.61 | +6.0% |
| identity + index 带回调读 | 71.48 | 71.25 | -0.3% |
| identity + index 带回调写 | 67.41 | 72.51 | +7.6% |
| identity + index 带两个 pre_access 和回调的写 | 73.91 | 79.04 | +6.9% |

同一脚本额外比较第二阶段 `29512da` 和最终版。每个场景 6 轮配对，单位 ns/次。

| 请求 | 29512da | 最终 | 变化 | 最终更快轮数 |
|---|---:|---:|---:|---:|
| 映射无回调读 | 71.67 | 69.87 | -2.5% | 5/6 |
| 映射无回调写 | 65.35 | 69.44 | +6.2% | 0/6 |
| 映射带回调读 | 70.44 | 71.99 | +2.2% | 1/6 |
| 映射带回调写 | 67.81 | 72.20 | +6.5% | 0/6 |
| 映射带两个 pre_access 和回调的写 | 73.78 | 78.16 | +5.9% | 0/6 |

映射写入的 4.1～4.4ns 增量在第三阶段出现，有无寄存器回调都能复现。
身份和下标 resolver 的成功路径现在也检查显式错误结果。
同一编译器生成的 `try_mmio_write_sized` 主体由 2,294 增至 2,773 字节。
按需 info 的展开位置保持一致。配对结果定位到阶段 3，尚无指令级采样可以量化各检查的成本。

`measure_mmio.py` 另外测随机与映射地址流，含 logger 开关与回调开关，四轮配对。
每个进程按四个引擎、八轮顺序轮换，每次遍历十万请求，执行三次遍历。无 logger 的
纯地址空间随机读 19.79→19.38ns，写 18.82→19.23ns。带回调的映射读
70.69→71.20ns，写 67.20→71.35ns。启用 logger 时，带回调的映射读
107.26→112.73ns，写 107.22→113.48ns。

`storage4096` 重复构造中位数 37.85→35.24ms，finish 约 0.05→0.06ms。
POE 形状中，重复构造 1.92→2.01ms，finish 0.56→0.54ms，分配器占用
3,319,552→3,313,360B。宽稀疏数组构造 12.00→12.51ms。性能结果来自库内基准，
实际设备的 QEMU 层调用、日志输出及业务回调需要在部署环境另测。

各场景的跨轮中位数、配对方向计数、校验结果和二进制指纹见
[基准结果 JSON](../benchmarks/results-architecture-sync-20261122.json)。

## 构建与兼容验证

| 配置 | 范围 | 结果 |
|---|---|---|
| GCC 14.2.0 Release，`-O3 -DNDEBUG -Wall -Wextra -Werror` | 每个阶段独立编译和测试 | 各 35/35 |
| Clang 19.1.7 Release，`-Wall -Wextra -Werror` | 最终版 | 35/35 |
| GCC ASan + UBSan，ASan 泄漏检测开启 | 最终版 | 35/35 |
| `cmake --install` + 独立 `find_package(regfw)` 消费者 | 三个独立翻译单元编译、链接和运行 | 通过 |

安装的静态库、GCC 测试使用的静态库和候选基准链接的静态库 SHA-256 一致。
