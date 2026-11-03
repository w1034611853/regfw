# 布局诊断与访问语义

[返回 README](../README.md) · [完整示例](../examples/pf_vf.cpp)

## 先看布局，再解释地址

从构建目录运行：

```sh
./regfw_pf_vf --layout
./regfw_pf_vf --layout-full
./regfw_pf_vf --explain
```

`--layout` 只显示 View / Entity 大范围、重复实例族和优先级；`--layout-full` 增加紧凑的寄存器地址表。多维数组只列一个地址公式以及 dims/strides/可见选择，可见选择相同的多 VF 只展开一个代表并标数量/stride；实例特有的覆盖保留为独立行。

直接调用 `map.describe_layout()` 得到 Summary；传入 `reg::LayoutDetail::Full` 得到详情。`finish()` 使用 hlog：Debug 自动打印 Summary，Trace 以 TRACE 等级打印 Full，Info 默认不显示布局。两者都不执行业务 callback。

当前代码生成的输出：[PF/VF 大布局](output/pf-vf-layout.txt) · [紧凑寄存器地址表](output/pf-vf-layout-full.txt) · [地址解释](output/pf-vf-explain.txt)。这些文件由编译后的示例生成，CTest 和发布脚本会逐字比对；仅将声明位置的 SDK 绝对路径前缀移除，以便解包、搬移后复现，文件名和行号仍校验。

表名从 `include/regfw/layout_titles.hpp` 生成：

<!-- GENERATED LAYOUT SECTIONS BEGIN -->
| 输出 | 内容 |
|---|---|
| BUNDLE / ADDRESS LAYOUT | Summary：地址 View 与共享实体引用；Full 同样保留。数组 REG 仅在 Full 的紧凑表显示 |
| STATIC ALIAS MAP | Full：静态别名、地址公式和权限 |
| SHARED STORAGE / STATIC ADDRESS TARGETS | Summary：共享存储及各个入口 |
| CANONICAL ADDRESS REGIONS / SHARED ACCESS | Full：规范地址区间及共享入口 |
| ACCESS PATHS → SAME CANONICAL RANGE | Summary/Full：汇合到同一存储区间的连线 |
| IDENTITY WINDOWS | Full：身份窗口、domain 和 resolver |
| INDEX MAPPING RULES (one representative per repeat) | Full：数组下标规则、条件与 fallback |
| REGISTERED MAPPING FUNCTIONS | Full：resolver 名称、绑定状态和注册位置 |
| EFFECTIVE INSTANCE ROUTING (symbolic; no resolver is executed) | Summary/Full：动态实例的符号路径，不执行 resolver |
| LAYOUT WARNINGS | 有警告时显示：诊断 code、原因与声明位置 |
<!-- GENERATED LAYOUT SECTIONS END -->

身份表中的 Request/Resolved 描述访问实例，不表示新建了额外存储。动态路由只能静态展示公式，不能靠打印函数得知当前设备 resolver 的返回值。

## explain_address

<!-- GENERATED EXPLAIN BEGIN -->
```cpp
// VF2 的 QUEUE[1][2]；只解释规则，不调用设备路由函数。
const auto address = vf_window(2) + HEAD_SIZE + 0x128;
std::cout << map.explain_address(address);
// 假设 resolver 返回 6，预览对应实体；数组映射仍以符号规则显示。
std::cout << map.explain_address(address, 4, reg::RegTransactionKind::Read, 6);
```
<!-- GENERATED EXPLAIN END -->

`explain_address(address, size=4, kind=Read, resolved_identity={})` 展示 bus → window/alias → 身份 → 寄存器 → 下标规则与事务要求。

它不会执行 identity/index resolver、predicate、行为或回调，不修改存储或事务快照。未知动态身份显示请求分支及公式；传入 resolved ID 是调用者作出的假设，数组映射仍以符号规则展示。它不能代替实际 MMIO 的状态检查。

## Entity / View 组合的诊断

`address_space` 的 Summary 只输出 View/Entity 排布，Full 增加 `VISIBLE REGISTER MAP (compact arrays)`，列出实际可见寄存器；不会逐数组元素或逐 32-bit beat 展开。部分覆盖显示可见数量/选择/partial 标记，公式并不意味着连续覆盖。真实遮挡产生 `REGFW_ENTITY_SHADOWED`，而 View 范围重叠但 REG 互补不会告警；最高优先级仍存在多个实际目标时报配置错误。`layout_warnings()` 可查询，不要求 logger；`finish()` 通过 hlog WARN 打印。完整语义见 [AddressViews](AddressViews.md)。

## 带事实的配置错误

声明保存 `std::source_location`。布局冲突列出双方名称、绝对区间、大小及各自文件/行号。错误中的区间用半开形式 `[start, end)`；布局表的 `a..b` 地址范围则包含最后一个字节 b。

`map.layout_warnings()` 返回 code、subject、message、location。`finish()` 在 Warn 输出警告；有警告时布局表包含 `LAYOUT WARNINGS`。

`REGFW_MISSING_IDENTITY_DOMAIN` 表示 access group 的 `identity_domain(N)` 条件没有对应的 identity window。fallback 可能是有意设计，因此是警告。检查只覆盖框架记录的命名 domain，不执行用户 predicate，也不推断函数内部依赖；声明了 domain 并不保证所有入口都能到达它。

## MMIO 的值与状态

| 情况 | 结果 |
|---|---|
| 正常读写 | Ok |
| `reg::ro` 写 / `read_only_ignore_writes()` 写 | Ignored；不修改值 |
| `read_only_error_writes()` 写 | AccessError |
| 保留区 / 未实现寄存器空洞 | DecodeError |
| 命令 REG 未绑定读/写回调 | 执行实体自身 behavior；不因缺回调而报错 |
| resolver 缺失绑定或普通异常 | DeviceError |
| 身份越界或显式身份拒绝 | 默认 AccessError；显式配置 Ignore 后读写都返回 Ignored |
| 宽寄存器未从所需首 beat 开始读 | Unsupported，并记录带寄存器名和所需偏移的 Warn |

`try_mmio_read_sized(addr, size)` 同时返回状态与 value；便捷 `mmio_read()` 只返回数值。身份 Ignore 下的读值是 `INVALID_READ_VALUE`。读值回调异常另有既存语义：返回 callback error magic，MMIO 状态可能仍为成功；不能用该结果判断 resolver 异常。QEMU 接口把 Ok / Ignored 都映射为 MEMTX_OK。

寄存器 RO 写仍校验访问宽度，但跳过 index resolver、pre_access、事务暂存、写行为和 on_write；身份 resolver 仍可能先执行以确定落点。窗口权限拒绝早于寄存器宽度校验。混合字段/自定义 behavior 的部分位未变，不等于整个请求 Ignored。

`mmio_write()` / `mmio_read()` 固定是 4 字节；8 字节访问明确使用 `mmio_write_sized(addr, value, 8)` / `mmio_read_sized(addr, 8)`。写值超过 size 位宽会 Warn，只写低位，不会自动猜测宽度。

## 统一 transaction 与宽寄存器

宽寄存器默认 LOW_TO_HIGH，首读必须覆盖低端；HIGH_TO_LOW 首读覆盖高端，INDEPENDENT 没有首 beat 限制。建立快照后中间 beat 不要求严格顺序，读到完成端结束快照。64 bit 和其他位宽统一使用 transaction；64 bit 默认 INDEPENDENT，>64 bit 默认 LOW_TO_HIGH。

合法的 4 字节宽寄存器访问和只写命令实体不会一律产生 lint 警告。实际访问出错时，优先检查返回状态和 Warn 日志，再用 explain 确认静态规则。

紧凑寄存器地址表按每行代表的最小实际可见入口地址进行 64 位数值升序排序；数组仍只列一行。部分覆盖时不使用可能已隐藏的数组首地址作为排序依据。

布局表中的地址映射行统一显示 `VIEW`；映射实体时显示 `VIEW → entity: <原始实体组或 REG 名称>`，`reset` 表示目标存储的复位归属。PF、PF_VF、VF 的多个入口会显示同一个共享实体目标名，不再把每个入口标成新 ENTITY。折叠重复族的目标名/复位信息取 i=j=k=0 代表实例；实体范围列是映射边界，不表示整段地址都有实体寄存器。

0.24.0 Full 增加 VIEW PRE_ACCESS BINDINGS；explain_address 静态列出选中路径的钩子键及绑定状态，不执行钩子。Summary 的 View 行显示 pre_access 键名。全局与 View 钩子异常语义相同，日志含失败 View/键名；异常停止后续阶段但不保证返回总线故障。
