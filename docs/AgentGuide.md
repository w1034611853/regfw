> 0.33.3：前端 API 保持；只声明寄存器的头可改用 <regfw/bundle.hpp>，见 [LayoutCompilation](LayoutCompilation.md)。当前测量见 [Performance](Performance.md)。

# regfw 0.33.3：面向 Agent 的使用说明与 API 契约

当前开发树的可选异步 SDK 见 [Async](Async.md)。端点在短状态锁内执行本文描述的原访问语义；耗时业务显式 defer，后台通过 with_state 更新 Bank。普通 RW、RC/FIFO、读返回值和动态路由保持即时语义，不能在状态锁内执行 DMA 或等待 worker。

复杂回调拆分、FIFO pop/空值/后台 push、每 executor 一个 worker 和当前验收缺口，见 [异步设备改造指南](Async-Device-Guide.md)。

适用版本：**regfw 0.33.3，配套 hlog 3.2.1，C++20**。本文根据该发布包的公开 API、运行时实现及测试核对编写，供编写设备模型的 Agent 直接使用。本文描述当前行为，不把建议或未来能力写成已有 API。

本文可以作为 SDK 根目录的新 README 使用。使用本版头文件时必须同时链接本版编译库；本版**不是 header-only 库**。不要从旧 Rust 实现、旧版 C++ 文档或通用寄存器框架经验推测接口。


**低上下文使用方式**：首次只读 §0～§2，按 §1 选择方案，再按任务读取下面对应章节。不要每次从头读到尾。章节标题可用于全文搜索；本文的完整示例不依赖其他文档。

| 当前任务 | 必读章节 |
|---|---|
| 新建普通寄存器、位域、数组 | §3～§7 |
| 512 bit 命令、同地址读写两种功能 | §7～§9、§17 |
| PF/VF 地址布局、共享实体 | [AddressViews](AddressViews.md)，再按需读 §10～§12 |
| VF 身份、虚拟下标转换 | §11、§12、§14 |
| 命令实体、doorbell | §13、§15 |
| 查默认策略、分拍触发条件 | §4、§8 |
| 错误、日志、trace 字段 | §14、§15、§18 |
| 复位、迁移、Host 并发 | §16、§17 |
| 工程链接、QEMU C 接口 | §20 |
| 写法报错、从旧版迁移 | §21、§22 |

## 0. Agent 首先遵守的规则

1. 新代码使用无地址实体；复杂布局优先使用 [AddressView / AddressSpace](AddressViews.md)。旧 Window DSL 已删除，本文完整程序均使用 View。
2. 同一实体多次 `.map(...)` 仍然只有一份值、回调、复位配置和快照内容；重复 `REG_ENTITY` 或 `REG_REPEAT_ENTITY` 才会创建独立存储。
3. 实体归属、地址入口、访问方向、VF 身份、数组下标是不同概念，不要互相替代。
4. `reg::at(offset)` 是实体组内的**字节偏移**；view.map 的 offset 是相对当前 View 的**字节偏移**。不要重复加基址。
5. `REG(NAME,T,...)` 声明一份标量或一整个数组实体，成员是 `RegView`；不要手工创建并 bind 另一套 view 数组。
6. `.ref()`、原生 union 成员、`.data()` 属于内部值访问，跳过 MMIO 权限、路由、behavior 和回调。
7. 数组的回调属于整个 `REG` 实体；通过 `.at(...)` / `.subspan(...)` 绑定回调不会建立独立元素回调。
8. `on_read` 可以返回完整值 `T`、`std::optional<T>`、`std::nullopt` 或 `void`。返回值用于读响应，不自动写回实体。
9. `on_write` 的 `(value, info)` 形式中，`value` 是**写入前的旧值**，不是新值。新值取 `info.stored_after`、宽值视图，或同步回调内的实体 `.ref()`。
10. ≤64 bit 默认逐次独立访问；>64 bit 默认 `TransactionPolicy::LOW_TO_HIGH`。512 bit 命令应声明为**一个 512 bit REG**，不是 16 个独立 uint32 寄存器。
11. 宽事务按完成端地址触发，不检查每个字节是否都访问过；软件必须遵守分拍协议。
12. 命令、doorbell、FIFO 端口也必须生成 REG 实体；不存在独立 command 解码区。按实际协议声明位宽、数组和 transaction。
13. 同一 `RegisterMap` 不支持未经同步的 Host 多线程访问；普通 bool 重入检查不是互斥锁。不要在回调中递归调用同一 map 的 MMIO。
14. `finish()` 冻结布局并检查 resolver 绑定；普通寄存器回调不是必需项，可在 finish 后首次绑定，但同一实体同种回调不能重复注册。
15. 回调异常会被捕获；有 logger 且 ERROR 启用才会打印。普通读值回调抛异常可能仍返回 `Ok`，值为 0；不要只靠 `.success()` 判断业务成功。
16. 不要使用已删除的 CommandPort、command window、REG_COMMAND_REGION 或 Split64 API，也不要虚构 `.on_write_complete()`、`RegView.bind()`、Rust 风格字段访问器或内部 inc/dec API。本文给出的名称才是本版可用写法。
17. 完整快照使用 bank.save/load 或版本化接口，包含进行中事务；旧 dump_state(span)/restore_state(span) 仍只处理值。接原有 vmstate_fields/module_var_*，不要求 VMStateDescription。

## 1. 按需求选择功能

| 需求 | 使用方式 | 关键约束 |
|---|---|---|
| 普通寄存器、计数器、状态 | `REG` + 内建 behavior / `on_read` | 外部和内部访问语义不同 |
| 每个 VF 独立存储 | `REG_REPEAT_ENTITY(vf, ..., N, ...)` | `vf[i]` 是独立实体组 |
| PF/VF 多入口共享唯一存储 | 单次 `REG_ENTITY`，多次 View `.map(target)` | 复位归属由实体配置决定 |
| 模块寄存器给 VF 只读 | `.map(module).read_only_ignore_writes()` | 只限制该入口，实体内部仍可修改 |
| 同形重复地址入口 | AddressView 的 `.map_repeat(name,offset,repeat,factory)` 或 AddressSpace 的 `.map_repeat(name,base,repeat,factory)` | factory 返回 AddressView，并映射 vf[i]；旧 Window DSL 已删除 |
| 虚拟 VF → 有效 VF | `.identity(domain).using_resolver(key)` | 返回同一窗口组内合法实例编号 |
| 虚拟数组下标 → 物理下标 | `REG_ACCESS_GROUP` + `reg::map_index` | 命名维度从 0 开始；身份先于下标转换 |
| 64 bit 两拍快照/提交 | `.transaction(...)` | 与所有宽度共用同一套事务状态 |
| 128/256/512…bit 命令 | 一个完整宽度 `REG` + `.transaction(...)` | 软件保证访问起止顺序和完整性 |
| doorbell / 命令地址 | 普通 REG 实体 + on_write/on_read | 不存在无实体命令解码；未声明地址为 DecodeError |
| 同地址写命令、读 FIFO 响应 | 一个宽 REG，独立的读源和写事件 | 读必须返回响应，避免 nullopt 回退到写存储 |
| PF 复位、VF FLR | `reset_scope` + `reset_domains` + `reset_matching` | 映射入口不改变实体复位 scope |
| 保存/恢复寄存器 | `bank.save/load` / 版本化快照 | 包含进行中事务；业务 FIFO、回调另行管理 |
| 检查布局或解释地址 | `describe_layout` / `explain_address` | 不执行设备 resolver，不产生访问副作用 |

## 2. 核心对象与生命周期

| 对象 | 它是什么 | 它不是什么 |
|---|---|---|
| `RegisterBank<Root>` | 同时拥有 map 和根 bundle 的推荐入口；不可复制，可移动 | 全局 singleton |
| `RegisterMap` | 持有实体、解码、路由、事务和诊断；不可复制/移动 | 线程安全的并发总线 |
| `RegEntity` | 一条 REG 的真实存储、数组形状、回调和复位状态 | 一个地址窗口 |
| `RegView<T,R,V>` | 指向实体或元素范围的非拥有、带类型句柄 | 另一份寄存器值 |
| `RegEntityBundle` | 存储实体组的声明容器 | 自动分配总线地址的容器 |
| `Bundle` | 推荐根布局类型 | 自动等同于 PF 或 VF |
| `AddressView` / `AddressSpace` | 局部布局与总线入口；复制布局不复制实体 | 存储拥有者 |
| `TypedReg` | `RegView` 的兼容别名 | 新增的一套运行时类型 |

推荐初始化顺序：构建 bank → 绑定 identity/index resolver → 绑定回调 → `bank.finish()` → 对外提供 MMIO。

`bank->xxx` / `(*bank).xxx` 取得 bundle 成员；`bank.map()` 取得 map。`reg::space(size).name(name).root(root_name).logger(logger).reset_scope(scope)` 设置构建参数。`.finalize()` 可以在构建时直接 finish，但有必需 resolver 时通常不适合，因为还来不及绑定。

bank 移动时底层对象地址保持稳定；现有 view 和 C bridge 仍指向同一底层对象。`bank.clear()` / 销毁会使 view 失效，受检访问会抛异常；之前取得的原生引用/指针仍遵循 C++ 生命周期规则，不能指望框架保护悬空引用。lambda 捕获的设备、FIFO、logger 必须活得足够久；临时 `info` 和其中的引用不能保留到异步任务。

## 3. 完整示例 A：最小布局、普通回调、内部访问

以下代码是独立可编译程序。`CONTROL` 的总线地址为 `0x100 + 0x00`；`STATUS` 为 `0x100 + 0x04`。

<!-- runnable: basic -->
```cpp
#include <regfw/regfw.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>

struct CoreRegs : reg::RegEntityBundle {
    REG_BUNDLE(CoreRegs);
    REG(CONTROL, std::uint32_t, reg::at(0x00).reset(0));
    REG(STATUS, std::uint32_t, reg::at(0x04).access(reg::ro).reset(7));
    REG(COUNTERS, std::uint32_t,
        reg::at(0x20).array(reg::dims(4), reg::strides(4)));
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(core, CoreRegs);
    REG_LAYOUT([&] {
        return reg::address_space("DEVICE")
            .map(reg::view("CORE", 0x100).map(core), 0x100);
    });
};
static_assert(CoreRegs::COUNTERS_address.offset(2) == 0x28);
static_assert(reg::PhysicalAddressWindow{0x100,0x100}
    .addr(CoreRegs::COUNTERS_address,2) == 0x128);
int main() {
    reg::RegisterBank<Layout> bank(reg::space(0x1000).name("DEVICE"));
    unsigned writes = 0;
    std::uint32_t model_status = 99;
    bank->core.CONTROL.on_write(
        [&](std::uint32_t old, const auto& info) {
            if (old != 0 || info.stored_after != 42)
                throw reg::Error("unexpected CONTROL transition");
            ++writes;
        });
    bank->core.STATUS.on_read([&]() -> std::uint32_t {
        return model_status;
    });
    bank.finish();
    auto& map = bank.map();
    if (!map.mmio_write(0x100, 42).success() || writes != 1)
        throw std::runtime_error("write failed");
    if (map.mmio_read(0x104) != 99 || bank->core.STATUS.ref() != 7)
        throw std::runtime_error("read source must not overwrite storage");
    if (map.mmio_write(0x104, 1).result != reg::RegMemTxResult::Ignored)
        throw std::runtime_error("RO write should be ignored");
    bank->core.COUNTERS.at(2).ref() = 123; // 内部访问，无 MMIO 回调
    if (map.mmio_read(0x128) != 123)
        throw std::runtime_error("array address mismatch");
    std::cout << "PASS basic\n";
}
```

## 4. REG 声明、类型、默认值

### 4.1 声明形式与配置表

```cpp
// 片段：放在含 REG_BUNDLE(...) 的实体组中。
REG(NAME, std::uint32_t,
    reg::at(0x40).reset(0).access(reg::rw).mask(0xffffffffu));
REG(TABLE, std::uint32_t,
    reg::at(0x100).array(reg::dims(2, 4), reg::strides(0x20, 4)));
```

`REG` 的 spec 必须可用于 `static inline constexpr`；含运行时 behavior 工厂等非 constexpr 内容时，使用同参数形式的 `REG_DYNAMIC`。不要把捕获设备实例的 lambda 写入静态 spec；设备相关逻辑通常在构建 bank 后绑定回调。

| 配置 | 默认 | 含义 |
|---|---|---|
| `.reset(value)` | 0 | 构建初始化和 reset 使用的值 |
| `.mask(value)` | 全部位可写 | 限制外部写入可改变的位，不限制内部直接赋值 |
| `.access(behavior)` | `reg::rw` | 内建/混合/自定义读写行为 |
| `.array(reg::dims(...), reg::strides(...))` | 标量，1 个元素 | 数组维度及**字节** stride；不推断紧密排列 |
| `.access_widths(policy)` | `LEGACY`，允许 1/2/4/8 字节候选访问 | 还会执行对齐、实体边界等检查 |
| `.transaction(policy)` | ≤64 bit：INDEPENDENT；>64 bit：LOW_TO_HIGH | 分拍快照/提交策略 |
| `.reset_domains(domains)` | `ResetDomains::STANDARD`，全部 6 个标准域 | 哪些复位事件匹配该实体 |
| `.reset_scope(scope)` | 继承实体组 / space 的 scope；最外层默认 Global | Global / Pf / Vf(id) |
| `.log(policy)` | `LogPolicy::Normal` | Normal / Trace / Hidden |
| `.snapshot(kind)` | `SnapshotKind::Migratable` | Migratable 才进入框架快照 |
| `.snapshot_stable_id("id")` | 实体声明名 | 稳定快照标识；不是自动迁移转换规则 |
| `.snapshot_order(n)` | 未指定 | 快照排序键 |
| `.defer_initial_reset()` | 不启用 | 构建时值初始化为 0，后续 reset 仍使用 reset(value) |

### 4.2 支持的值类型

- 标量：8/16/32/64 bit 整数；推荐 `std::uint32_t` / `std::uint64_t`。
- 宽值：`reg::WideUInt<N>`，REG 存储要求 `64 < N <= 2048` 且 `N % 8 == 0`。常用别名：`reg::u128/u256/u512/u1024/u2048`。
- 原生 union/struct：需平凡可复制、标准布局，具有受识别 raw 成员，且 raw 成员覆盖整个对象，无额外 padding。推荐 uint32/uint64 raw 和位域 union。
- 枚举和具有既有 raw 编解码契约的包装类型也受支持；新模型优先用整数、WideUInt 或原生 union，减少自定义适配。

```cpp
union Status32 {
    std::uint32_t u32;
    struct {
        std::uint32_t ready : 1;
        std::uint32_t count : 15;
        std::uint32_t reserved : 16;
    } bits;
};
union Packet512 {
    std::uint32_t u32[16];
    std::uint64_t u64[8];
};
static_assert(sizeof(Status32) == 4);
static_assert(sizeof(Packet512) == 64);
```

支持识别的 raw 成员名：`u8/u16/u32/u64/u128/u256/u512/u1024/u2048`；例如 `u32[16]` 表示 16 个 32 bit word，覆盖一个 512 bit 值。多个 raw 视图必须覆盖同一完整位宽，不能一个视图只有低 32 bit、另一个覆盖 512 bit。`bits` 是用户自己的 C++ 位域成员，不是框架自动生成的字段 API。C++ 位域布局受 ABI/编译器影响，设备工程需匹配预期平台。

`reg::WideRegData` 是框架内部通用 2048 bit 值类型；声明 512 bit REG 时优先用 `reg::u512` 或自己的完整值类型，不要误用 WideRegData 导致实体变成 2048 bit。

WideUInt 可用：`word(i)` / `set_word(i,v)`（64 bit word，0 为最低端）、`bit(i)` / `set_bit(i,v)`、`from_hex`、`from_words`、`mask(n)`、`low_u64()`、`checked<U>()`、`truncated_to<N>()`、位运算、移位、加减和 `hex()`。缩窄转换默认检查溢出；主动截断须明确使用截断 API。不要假设存在乘除、动态位段代理或 Rust 的 `.bits(a..b)`。

### 4.3 地址与访问的硬约束

- map 大小必须非零、4 字节对齐；已支持 uint64_t 地址。`space()` 默认上界为 `UINT64_MAX & ~uint64_t{3}`（半开范围，无法表示长度 2^64）。地址可使用完整 guest 物理地址，也可使用设备相对地址，入口和布局必须采用相同坐标。>4 GiB 范围按需分配稀疏页，不按巨大跨度分配目录。
- 寄存器声明基址必须 4 字节对齐；8/16 bit 实体在解码布局里也至少占一个 4 字节槽位。
- 单寄存器最大 2048 bit；不是无限位宽。
- 数组维度数为 1..8；维度非零，总元素数受 uint32 范围检查及实际资源限制；不能把其他项目的 1,048,576 元素/256 MB 限额当成本包保证。
- stride 是字节数且必须 4 字节对齐；布局不得导致真实存储字节冲突。
- 单次 MMIO 仅接受 1/2/4/8 字节；地址需按 size 自然对齐，不得跨实体边界。8 字节访问还要求映射后的两个 4 字节解码 word 属于同一实体元素且偏移连续。
- View 的 offset、size 均需 4 字节对齐；size 非零，子 View 必须落在父 View 内。
- 不是 32 bit 整倍数的宽值可能需要末尾 1/2 字节访问；软件协议必须选择合法拍宽并覆盖完成端。512 bit 用 4/8 字节拍即可。

## 5. behavior：值怎么变化

behavior 负责存储值转换；`on_write` / `on_read_complete` 是转换后的业务事件。不要用外部回调重复实现已配置的 W1C/RC 等行为。

| `.access(...)` | 写入行为 | 默认读取行为 |
|---|---|---|
| `reg::rw` | 访问掩码内替换为写值 | 返回存储值 |
| `reg::ro` | 返回 Ignored，不改值，不触发 on_write | 返回存储值 |
| `reg::w1c` / `reg::wc` / `reg::int_wc` | 写 1 清相应位；写 0 保持 | 返回存储值 |
| `reg::w1s` | 写 1 置相应位 | 返回存储值 |
| `reg::w0c` | 写 0 清相应位 | 返回存储值 |
| `reg::w0s` | 写 0 置相应位 | 返回存储值 |
| `reg::rc` | 当前内建写行为保持原值 | 返回旧值，并清除本次 behavior 覆盖位 |
| `reg::write_only` | 普通可写 | 候选读值为 0；不等于禁止 on_read 提供响应 |
| `reg::w1_pulse` | 当前内建实现不保留写值，可在 on_write 使用请求数据 | 返回存储值；无自动脉冲调度器 |
| `reg::rwsc` | 当前实现等同 RW | 无自动延时自清零任务 |
| `reg::rwsc_immediate` | 清除访问到的可写位 | 返回存储值 |
| `reg::normal_read` / `reg::cnt_cyc` | 分别同 RW / RO | 分别同 RW / RO |

RC 宽事务在首拍按完整寄存器行为计算并清值，再把候选读结果冻结为快照；不是到 read_complete 才清实体。若业务必须读完才出队，在 `on_read` 生成/窥视响应，在 `on_read_complete` 执行出队，不要依赖 RC 替代 FIFO 协议。

混合位行为可声明：

```cpp
// 片段：低 8 bit 为 RW，接下来 8 bit 为 W1C，高 16 bit 为 RO。
REG(MIXED, std::uint32_t,
    reg::at(0x80).access(reg::mixed(
        reg::rule(0x000000ffu, reg::rw),
        reg::rule(0x0000ff00u, reg::w1c),
        reg::rule(0xffff0000u, reg::ro))));
```

混合规则 mask 不能重叠；未覆盖位保持写值不变、读取仍可见。整个寄存器不是内建纯 RO 时，不能因为最终值没有变化就推断不会执行 on_write。

自定义 behavior：`reg::FieldBehavior` 的 write 签名为 `RegData(current,value,mask)`，返回转换后的值；read 签名为 `RegData(RegData& stored,RegData mask)`，可修改 stored 并返回候选读值。宽版本是 `reg::WideFieldBehavior`，参数改为 WideRegData。通过 `REG_DYNAMIC(..., reg::at(...).access(behavior))` 使用，通常只在必须改变存储变换规则时选择。behavior 执行期间同一实体被 borrow 保护，不能递归访问它；设备业务优先放回调中。

## 6. 内部访问、数组与 view

| API | 含义 |
|---|---|
| `reg.ref()` | 值引用，仅标量/元素 view 可用 |
| `reg.u32` / `reg.bits.ready` | 存在对应原生成员时直接访问值 |
| `array.at(i,j,...)` | 按完整多维坐标取得元素 view；参数个数等于 rank |
| `array.at_element(n)` | 按当前 view 内的展平编号取得元素 view |
| `array.at_element(info.index)` | 按类型化索引取得元素，验证形状和范围 |
| `array.subspan(offset,count)` | 仅一维；取得范围 view，不创建实体 |
| `.size()` / `.empty()` | 当前 view 的元素数量/是否为空 |
| `.data()` / `.size_bytes()` | 连续 C++ 值存储指针和其字节数；空 view 指针为空 |
| `.address()` | 当前 view 首元素的规范总线地址，不是本次访问地址；未映射/空 view 报错 |
| `.address_span_bytes()` | 包含地址 stride 空洞的总线跨度 |
| `.entity()` | 真实实体身份、元素总数、位宽、维度、存储大小 |
| `.reset()` | 复位整个实体，不只当前元素或 subspan |
| `.is_alive()` | 检查句柄生命周期 |
| `.metadata()` / `.diagnostic_name()` | 获取声明/诊断信息 |

例如 `dims(2,4)` 的 `[1][2]` 展平编号为 `1*4+2=6`，无论总线 stride 是否紧密排列。`at_element(6)` 不是地址 `+6` 或第 6 个字节。对 `root.subspan(2,3)`，`at_element(0)` 是 root 第 2 个元素。

数组实体的回调和 reset 覆盖整个实体：不要循环对 `array.at(i).on_write(...)` 注册不同回调，第二次注册会因同一槽已有回调而失败。只绑定一次 `array.on_write(...)`，在 info.index 中取得有效坐标。

`.ref()` 返回 T&；`auto value = r.ref()` 因 auto 推导而复制值，`auto& value = r.ref()` 才保留引用。句柄之间赋值复制绑定；复制实体值应使用 `r1.ref() = r2.ref()`。

内部访问跳过地址别名、VF 解析、下标转换、权限、behavior、日志及回调。`bank->vf[6].QUEUE.at(1,3).ref()` 已经明确指定实体与元素，不再做 VF 或 pool 转换。内部引用必须由设备工程同步，框架不会自动加锁。

## 7. 回调签名与精确契约

### 7.1 REG 回调的合法形状

设 REG 的完整值类型为 `T`，rank 为 `R`。推荐写泛型 info，让框架推导类型；也可写 `const reg::RegAccessInfo&`，取得公共原始 info。

| API | 支持参数形状 | 返回值 |
|---|---|---|
| `.on_read(f)` | `(T candidate,const auto& info)`、`(const auto& info)`、`() ` | `T`、optional<T>、nullopt、void |
| `.on_write(f)` | `(T old,const auto& info)`、`(const auto& info)`、`() ` | 作为事件使用；推荐 void，不把返回值当数据 |
| `.on_read_complete(f)` | `(T snapshot,const auto& info)`、`(const auto& info)`、`() ` | 必须 void |

只接收一个值参数 `(T value)` 不是受支持的独立形状；使用 `(T value,const auto& info)`。同一个实体的每种回调只可注册一次；不能通过再次调用 on_read 覆盖旧函数。`has_on_read()` / `has_on_write()` / `has_on_read_complete()` 可查询。resolver 可以换绑，与普通回调的单次注册规则不同。

### 7.2 读取返回值

- `T` / optional 有值：作为本次响应/事务快照。
- `nullopt` / 空 optional / void：沿用 behavior 已经计算出的候选读取结果，不表示访问失败。
- 候选值在进入 `on_read` 前已确定；在回调里修改 entity 后返回 `std::nullopt`，不会重新读取修改后的实体值。本次要返回修改后的值，必须显式返回该值。这里的空返回是 `std::nullopt`，不是 `nullptr`。
- `on_read` 在 behavior 之后调用；此时 RC 等存储副作用可能已经发生。
- `on_read` 返回值不会自动更新 `.ref()`。需要更新实体时由模型明确更新，但不要为建立读快照而覆盖命令写存储。
- 512 bit REG 返回完整 512 bit T；本次 4/8 字节响应由框架提取对应片段。

```cpp
// 片段：始终提供动态值，不必返回 optional。
bank->core.STATUS.on_read([&]() -> std::uint32_t { return model_status; });
// 替代形式：有条件回退。两种写法选一种，不能给同一实体重复注册。
// bank->core.STATUS.on_read([&]() -> std::optional<std::uint32_t> {
//     if (use_model) return model_status;
//     return std::nullopt;
// });
```

**注意：修改实体与返回读取响应是两件独立的事。** 下面以普通 RW、32-bit 标量 REG 为例，两个回调是替代写法，不能对同一实体都注册：

```cpp
// status 是已声明的 std::uint32_t REG 句柄，实体原值为 5。
status.ref() = 5;
status.on_read([&]() -> std::optional<std::uint32_t> {
    status.ref() = 42;
    return std::nullopt;
});
// 本次 MMIO：QEMU 读到 5；实体 status.ref() 变成 42。
// 下一次读取的候选值才是 42。

// 替代回调：希望本次就读到新值，修改后显式返回。
// status.on_read([&]() -> std::uint32_t {
//     status.ref() = 42;
//     return status.ref();
// });
```

RC 等 behavior 可能已在回调前修改存储，例如 RC 的候选值为清零前的值、实体已清零，不能简单把候选值理解为回调时的 `.ref()`。对采用读取快照的分拍事务，上述选择发生在建立完整快照时；后续拍使用已建立的快照，不因 entity 后续变化而自动刷新。

### 7.3 写回调与读完成回调

`on_write` 在 behavior 已处理、实体已更新之后执行；首个 T 参数是旧值。普通寄存器的新值在 `info.stored_after`，宽寄存器使用完整宽值视图或 `.ref()`。异步提交时复制命令值，不要把实体引用作为任务数据：后续访问可能修改它。

`on_read_complete` 的 T 参数是本次实际响应的完整快照。不要从 `.ref()` 取读响应；那里可能仍是最近写命令。完成回调在处理最后一拍、返回该次 MMIO 之前同步执行，不表示 guest CPU 后续业务已经使用完数据。

所有回调在 MMIO 调用路径同步执行；没有自动设备线程、异步任务队列或延时调度器。设备自行安排后续任务。

## 8. 分拍事务：默认值、完成时机和边界

### 8.1 默认行为

| 位宽 | 默认 transaction | 结果 |
|---|---|---|
| 8/16/32 bit | INDEPENDENT | 每次访问独立执行 |
| 64 bit | INDEPENDENT | 高低半各自执行回调，不共享快照、不延迟提交 |
| >64 bit | LOW_TO_HIGH | 低端读建快照，高端写提交/读完成 |

所有位宽只使用 TransactionPolicy。64 bit 需要整笔行为时显式 LOW_TO_HIGH 或 HIGH_TO_LOW；512 bit 建议也显式写出，便于审查。单次 MMIO 仍是 1/2/4/8 字节，不能把 size 写成 64 来表示一次 512 bit MMIO。

### 8.2 TransactionPolicy

| 策略 | 首拍读取 | 读完成 | 写提交 |
|---|---|---|---|
| `INDEPENDENT` | 每拍调用 on_read | 每拍完成后调用 on_read_complete | 每拍更新、调用 on_write |
| `LOW_TO_HIGH` | offset==0 时取完整快照 | offset+size==寄存器字节数 | offset+size==寄存器字节数 |
| `HIGH_TO_LOW` | offset+size==寄存器字节数时取完整快照 | offset==0 | offset==0 |

支持分别配置读写模式：`reg::TransactionPolicy{reg::SplitReadMode::SnapshotOnLow, reg::SplitWriteMode::Independent}`；另外有 SnapshotOnHigh、CommitOnHigh、CommitOnLow。不是所有设备都必须读写同方向。

**完成端是协议约定，不是覆盖完整性的检查。** 写入 pending 和访问掩码在完成端一次提交；没访问的位保留实体旧值。仅写最高端也会提交。先读低端再跳到最高端，也会调用完成回调。中间拍不强制严格递增/递减。

读无有效快照且首拍不符合起始端要求：返回 Unsupported、INVALID_READ_VALUE，并可输出 Warn。活跃快照期间再次读取起始端会重新取快照。读源回调异常时完成事件不会正常触发。

### 8.3 64 bit 使用相同的 transaction

```cpp
// 片段：低端首读创建 64 bit 快照，高端读取完成；高端写入提交。
REG(TXN64, std::uint64_t,
    reg::at(0x40).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
// 只允许完整 8 字节 MMIO，通过访问宽度配置，不需要专用分拍策略。
REG(NATIVE64, std::uint64_t,
    reg::at(0x48).access_widths(reg::RegisterAccessWidthPolicy::U64_ONLY));
```

0.23.0 删除了旧的 Split64Policy/对应 builder 和专用状态，不保留并行实现。原“低端建快照、高端完成/提交”使用 LOW_TO_HIGH；反方向使用 HIGH_TO_LOW。只让一侧分拍时，明确构造 TransactionPolicy 的 read/write 模式，另一侧选 Independent。

原 latch 策略会在后续拍仍调用 on_read，再替换已锁存的另一半；新的 SnapshotOnLow/High 是首拍取一次完整快照，后续拍不重新调用 on_read。这是有意统一后的语义，不是假装旧 latch 副作用完全相同。

## 9. 完整示例 B：512 bit，同地址写命令、读 FIFO 响应

读源返回完整 Packet512，写命令保留在实体，读取响应独立存放在框架快照中。示例刻意让读事务与写事务交错：写前半 → 开始读快照 → 写后半并改变模型响应 → 继续读原快照。

<!-- runnable: duplex -->
```cpp
#include <regfw/regfw.hpp>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>

union Packet512 { std::uint32_t u32[16]; };
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(CMD, Packet512,
        reg::at(0).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
};
struct CommandLayout : reg::Bundle {
    REG_BUNDLE(CommandLayout);
    REG_ENTITY(commands, CommandRegs);
    REG_LAYOUT([&] {
        return reg::address_space("DEVICE")
            .map(reg::view("COMMAND", 0x40).map(commands), 0);
    });
};
void require(bool ok) {
    if (!ok) throw std::runtime_error("duplex check failed");
}
int main() {
    reg::RegisterBank<CommandLayout> bank(reg::space(0x1000));
    auto& cmd = bank->commands.CMD;
    Packet512 response{}, submitted{}, consumed{};
    unsigned writes = 0, reads = 0, completed = 0;
    for (unsigned k = 0; k < 16; ++k) response.u32[k] = 100 + k;
    cmd.on_write([&] {
        submitted = cmd.ref(); // 按值复制新命令；不是保存引用
        ++writes;
        // 模拟下发命令导致下一条 FIFO 响应变化。
        for (auto& word : response.u32) word = 999;
    });
    cmd.on_read([&]() -> std::optional<Packet512> {
        ++reads;
        return response; // 完整 512 bit；也可直接返回 Packet512
    });
    cmd.on_read_complete([&](Packet512 snapshot, const auto&) {
        consumed = snapshot; // 实际 FIFO 出队逻辑可放此处
        ++completed;
    });
    bank.finish();
    auto& map = bank.map();
    for (unsigned k = 0; k < 8; ++k)
        require(map.mmio_write(k * 4, k + 1).success());
    require(writes == 0 && cmd.ref().u32[0] == 0);
    require(map.mmio_read(0) == 100 && reads == 1);
    for (unsigned k = 8; k < 16; ++k)
        require(map.mmio_write(k * 4, k + 1).success());
    require(writes == 1);
    for (unsigned k = 1; k < 16; ++k)
        require(map.mmio_read(k * 4) == 100 + k);
    require(reads == 1 && completed == 1);
    for (unsigned k = 0; k < 16; ++k) {
        require(submitted.u32[k] == k + 1);
        require(consumed.u32[k] == 100 + k);
        require(cmd.ref().u32[k] == k + 1); // 读响应没有覆盖写存储
    }
    std::cout << "PASS duplex\n";
}
```

如果读写协议需要不同字段解释，可以在同一完整 raw 宽度上分别解析请求/响应；框架不会自动定义两份不同宽度的读写实体。若读窗口和写窗口的边界/位宽不同，一个 REG 不能分别声明两种 transaction 长度，需要设备适配或扩展。

## 10. View 布局：映射、重复、共享与保留区

0.23.0 已删除旧 Window DSL。公开布局入口只有 `reg::view(name,size)` 和 `reg::address_space(name)`，完整签名、优先级、地址规则见 [AddressViews](AddressViews.md)。

```cpp
// 已声明实体 vf[i] 和 shared；仅展示 REG_LAYOUT 内片段。
auto shared_view = reg::view("SHARED", BUSINESS_SIZE).map(shared);
auto business = [&](auto i) {
    return reg::view("BUSINESS", BUSINESS_SIZE)
        .map(vf[i]).map(shared_view, 0, 10);
};
return reg::address_space("DEVICE")
    .map_repeat("A", BASE_A, reg::repeat(VF_COUNT).stride(STRIDE), business)
    .map_repeat("B", BASE_B, reg::repeat(VF_COUNT).stride(STRIDE), business);
```

`View.map(target,offset=0,priority=0)` 支持 Entity/Bundle、REG/数组切片和其他 View。实体组保留内部 REG 偏移；REG/切片把首元素平移到 offset。目标 View 使用自身 size；REG/切片跨度由宽度/stride 推导；Entity 受当前 View 剩余范围约束。需要严格块边界时先用 view(name,block_size).map(entity) 包装。

`AddressView.map_repeat(name,offset,repeat,factory,priority=0)` 可以在局部 View 中建立重复族，之后整体挂载。`AddressSpace.map(view,base,priority=0)` 挂载物理入口；map_repeat(name,base,repeat(N).stride(S),factory,priority=0) 建立重复实例族。builder 都返回新对象，循环中必须赋回。工厂参数是整数实例下标。

空洞透明。优先级只在同级分支比较；高优先级实际 REG 覆盖低优先级 REG 时产生 REGFW_ENTITY_SHADOWED warning，同级最高优先级歧义报错。reserve(offset,size,priority) 是显式终止区；权限拒绝或选中目标失败不回退。

View 的 read_only_*、identity(domain).using_resolver(key)、vf_to_pf/pf_to_vf 修饰作用于该路径；不改变实体存储、复位和快照归属。同一实体多个入口共用回调和 transaction 状态。先重建布局并绑定解析器，再 finish。

## 11. VF 身份解析与数组下标转换

### 11.1 两种转换不要混淆

| 阶段 | 输入 → 输出 | 影响 |
|---|---|---|
| 静态映射 | 原始入口地址 → 规范地址 | 多入口共享实体 |
| 身份解析 | request VF i → effective VF j | 同一 repeat 组第 j 个窗口，保持窗口内偏移 |
| 下标解析 | 逻辑坐标 → 有效坐标 | 改变目标实体中的数组元素 |

View `.identity(domain).using_resolver(key)` 声明解析要求：

```cpp
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
// 片段：在重复窗口 factory 中。
// return reg::view("SLOT", stride)
//     .identity(VF_DOMAIN).using_resolver(VfRoute).map(vf[i]);
```

初始化时绑定：

```cpp
// 片段：route_table 是设备持有的有效编号表。
map.set_identity_resolver(VfRoute, "Device::route_vf",
    [&](const reg::IdentityResolveRequest& r) -> std::uint32_t {
        return route_table.at(r.request_id);
    });
```

identity key 类型必须 uint32_t。返回编号必须在该 repeat 组 [0,count) 内；不是全局设备 ID 或 PF sentinel。`r` 包含 domain、request_id、bus_offset、canonical_offset、offset_in_instance、access_size、route、source。后三个地址/偏移字段为 uint64_t，不会截断高地址。

resolver 把 i→j 后，框架重新解析第 j 个窗口相同偏移；`.map(vf[i])` 因此落到 vf[j]。若每个窗口的共享子区均 `.map(shared)`，无论 j 为何仍访问同一 shared。命令 REG 同样按有效身份 j 选择实体，一次访问只解析本次身份一次。

把 identity 挂到整个 VF_WINDOW，会覆盖其模块只读区、共享区、私有区和命令实体；resolver 失败会阻断这些访问。只希望部分区域转换时，把 identity 挂到该子窗口；它继承最近 repeat 的编号/数量/stride。PF_VF 和 VF 两组窗口都使用同一个 key，表示两者调用同一解析函数；希望 PF_VF 不转换则不要给该组挂 identity。

若只写 `.identity(domain)` 而不指定 key，使用 map 的全局 identity resolver；窗口编译器必要时提供 passthrough。推荐命名 key：`.using_resolver(VfRoute)` 会要求绑定，避免误以为已有设备路由。

### 11.2 数组下标映射

以下声明放在含 QUEUE 的实体组中，且必须位于对应 REG 声明之后：

```cpp
REG_ACCESS_FIELD(LogicalPool, std::uint32_t);
REG_ACCESS_FIELD(PhysicalPool, std::uint32_t);
// 片段：下列内容属于一个实体组。
// REG(QUEUE, std::uint32_t,
//     reg::at(0x100).array(reg::dims(2,4),reg::strides(0x20,4)));
// REG_ACCESS_GROUP(POOLS,
//     .members = {QUEUE},
//     .coordinates = {{LogicalPool, 1}},
//     .derive = {reg::map_index(LogicalPool, PhysicalPool)
//         .when(reg::identity_domain(VF_DOMAIN))
//         .using_resolver(PhysicalPool)
//         .otherwise(reg::passthrough(LogicalPool))});
```

`.coordinates={{LogicalPool,1}}` 指第二维；第一个元素维度是 0，第二维是 1，不是数组坐标值。命名 output 默认沿用 source 所属维度，resolver 返回值会替换这一维；`.metadata_only()` 只产生派生字段，不改变实际数组元素。

```cpp
// 片段：set_access_resolver 的回调接收公共 RegAccessInfo。
map.set_access_resolver(PhysicalPool, "Device::pool",
    [](const reg::RegAccessInfo& info) -> std::uint32_t {
        return (info.index_as(LogicalPool) + 1) % 4;
    });
```

`info.resolved_id()` 可取得 identity 已转换后的 VF；PF 未经过 identity 时不要直接调用，先检查 `info.has_identity()` / condition。`.when(identity_domain(...))` 为条件匹配；否则用 passthrough 保留逻辑坐标。

规则只作用于 `.members` 指定的实体，其他寄存器不会自动参加。members 必须是整个实体的 view，不能是 array.at(i) 或 subspan。支持多个规则/组，但同一维度若给出互相矛盾的有效下标会失败。输出必须非负、在目标维度范围内且符合声明字段类型。

使用 `info.index_as(LogicalPool)` 取得原始命名坐标，`info.mapped_as(PhysicalPool)` 取得派生值。`info.request_index` 是转换前完整数组坐标，`info.index` 是转换后完整坐标；`.get()` 对一维返回单个整数，对多维返回 tuple。`index=2` 这类日志字段是展平编号。

**下标转换直接选存储元素，不重编码 info.resolved_address。** 即使请求 [1][2] 实际访问 [1][3]，target 也可能仍显示原始解码地址；查实际落点请看 index。

### 11.3 resolver 生命周期与错误

- `finish()` 检查被布局引用的命名 identity/index resolver 是否绑定，即使某些条件运行时不命中，声明的依赖仍需满足。
- `set_identity_resolver(key,...)` / `set_access_resolver(key,...)` 可以在 finish 后换绑；新访问使用新函数。
- `clear_*_resolver(key)` 后若仍执行相关访问会失败；`has_*_resolver(key)` 查询绑定状态。
- `defer_identity_resolver(key)` / `defer_access_resolver(key)` 是 finish 前显式延迟绑定的特殊机制，不提供默认业务值；实际访问前仍应绑定。
- identity 越界默认 AccessError；`.set_identity_rejection_policy(reg::IdentityRejectionPolicy::Ignore)` 可让显式身份拒绝变为 Ignored，但 resolver 自身普通异常仍为 DeviceError。
- 普通 resolver 抛异常返回 DeviceError；可用 `reg::RegAccessDeriveError::access_error("...")` 等明确分类。不要使用普通回调抛异常来要求 MMIO 总线失败。

## 12. 完整示例 C：32 MiB PF/VF、全局共享实体、身份和下标路由

地址骨架：PF 为 [0,16 MiB)，VF 为 [16 MiB,32 MiB)；PF_VF 槽位从 2 MiB 开始，每个 1 MiB；VF 窗口每个 2 MiB。每个业务区从各入口的 +128 KiB 开始。共享 CPU/GROUP 只存在于 pf 实体组中，VF 通过子窗口引用它们。

<!-- runnable: pf_vf -->
```cpp
#include <regfw/regfw.hpp>

namespace example {
inline constexpr std::uint64_t KiB = 1024, MiB = 1024 * KiB;
inline constexpr unsigned VF_COUNT = 8;
inline constexpr std::uint32_t VF_DOMAIN = 1;
inline constexpr std::uint64_t SPACE_SIZE = 32 * MiB;
inline constexpr std::uint64_t HEAD_SIZE = 128 * KiB, BUSINESS_SIZE = 896 * KiB;
constexpr std::uint64_t pf_slot(unsigned i) { return 2 * MiB + i * MiB; }
constexpr std::uint64_t vf_window(unsigned i) { return 16 * MiB + i * 2 * MiB; }
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(LogicalPool, std::uint32_t);
REG_ACCESS_FIELD(PhysicalPool, std::uint32_t);

// DOC-BEGIN entities
struct ModuleRegs : reg::RegEntityBundle {
    REG_BUNDLE(ModuleRegs);
    REG(DEVICE_ID, std::uint32_t, reg::at(0x00).reset(0x6188).access(reg::ro));
    REG(STATUS, std::uint32_t, reg::at(0x08));
    REG(TIMESTAMP, std::uint64_t, reg::at(0x20).access(reg::ro)
        .transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(STATS, std::uint32_t, reg::at(0x80).array(reg::dims(4), reg::strides(4)));
};
struct PfRegs : reg::RegEntityBundle {
    REG_BUNDLE(PfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(CPU_SHARED, std::uint32_t, reg::at(0x40));
    REG(GROUP_SHARED, std::uint32_t, reg::at(0x80));
    REG(DESC_BASE, std::uint64_t, reg::at(0xc0));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(STATUS, std::uint32_t, reg::at(8).access(reg::ro));
    REG(QUEUE, std::uint32_t, reg::at(0x100)
        .array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG_ACCESS_GROUP(POOLS,
        .members = {QUEUE}, .coordinates = {{LogicalPool, 1}},
        .derive = {reg::map_index(LogicalPool, PhysicalPool)
            .when(reg::identity_domain(VF_DOMAIN))
            .using_resolver(PhysicalPool)
            .otherwise(reg::passthrough(LogicalPool))});
};
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(DOORBELL, std::uint32_t, reg::at(0x10).access(reg::write_only));
};
// DOC-END entities

// DOC-BEGIN windows
struct DeviceRegs : reg::Bundle {
    REG_BUNDLE(DeviceRegs);
    REG_ENTITY(module, ModuleRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(pf, PfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, VfRegs, VF_COUNT,
        reg::entity().reset_per_instance(reg::ResetScope::Vf));

    REG_ENTITY(pf_commands, CommandRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf_commands, CommandRegs, VF_COUNT,
        reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_LAYOUT([&] {
        const auto module_view = reg::view("MODULE", HEAD_SIZE).map(module);
        auto business = [&](auto i) {
            return reg::view("BUSINESS", BUSINESS_SIZE).map(vf[i])
                .map(pf.CPU_SHARED, 0x40, 10)
                .map(pf.GROUP_SHARED, 0x80, 10);
        };
        const auto pf_view = reg::view("PF", 16 * MiB)
            .map(module_view)
            .map(reg::view("PF_BUSINESS", BUSINESS_SIZE).map(pf), HEAD_SIZE)
            .map(reg::view("PF_COMMAND", MiB).map(pf_commands), MiB)
            .reserve(10 * MiB, 6 * MiB);
        auto poe_cpu = reg::view("DEVICE", SPACE_SIZE)
            .map(pf_view, 0)
            .map_repeat("PF_TO_VF", 2 * MiB, reg::repeat(VF_COUNT).stride(MiB), [&](auto i) {
                return reg::view("SLOT", MiB).identity(VF_DOMAIN).using_resolver(VfRoute)
                    .reserve(0, HEAD_SIZE)
                    .map(business(i).pf_to_vf(), HEAD_SIZE);
            })
            .map_repeat("VF", 16 * MiB, reg::repeat(VF_COUNT).stride(2 * MiB), [&](auto i) {
                return reg::view("VF_WINDOW", 2 * MiB).identity(VF_DOMAIN).using_resolver(VfRoute)
                    .map(module_view.read_only_ignore_writes().vf_to_pf())
                    .map(business(i).vf_to_pf(), HEAD_SIZE)
                    .map(reg::view("VF_COMMAND", MiB).map(vf_commands[i]).vf_to_pf(), MiB);
            });
        return reg::address_space("POE").map(poe_cpu, 0);
    });
};
// DOC-END windows
} // namespace example

// 完整示例：布局见 pf_vf_layout.hpp；此处绑定设备逻辑并验证实际访问。
#include <array>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace example {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Device {
    std::array<std::uint32_t, VF_COUNT> route{};
    unsigned route_calls = 0, index_calls = 0, command_calls = 0;
    unsigned last_request = 99, last_resolved = 99;
    std::uint64_t last_command_offset = 0, ticks = 0x1122334455667788ull;
    reg::RegisterBank<DeviceRegs> bank;

    Device() : bank(reg::space(SPACE_SIZE).name("POE")) {
        std::iota(route.begin(), route.end(), 0);
        auto& map = bank.map();
        // DOC-BEGIN callbacks
        map.set_identity_resolver(VfRoute, "Device::route_vf",
            [&](const reg::IdentityResolveRequest& request) {
                ++route_calls;
                return route.at(request.request_id);
            });
        map.set_access_resolver(PhysicalPool, "Device::route_pool",
            [&](const reg::RegAccessInfo& info) {
                ++index_calls;
                return (info.index_as(LogicalPool) + 1) % 4;
            });
        bank->vf[3].STATUS.on_read([]() -> std::optional<std::uint32_t> { return 777; });
        bank->module.TIMESTAMP.on_read([&] { return ticks++; });
        bank->pf_commands.DOORBELL.on_write([&](const reg::RegAccessInfo&) { ++command_calls; });
        for (unsigned i = 0; i < VF_COUNT; ++i) {
            bank->vf_commands[i].DOORBELL.on_write([&, i](const reg::RegAccessInfo& info) {
                ++command_calls;
                last_request = info.request_id();
                last_resolved = info.resolved_id();
                last_command_offset = info.resolved_address - (vf_window(i) + MiB);
            });
        }
        bank.finish(); // 绑定完设备逻辑后再校验并冻结布局。
        // DOC-END callbacks
    }

    void demonstrate() {
        auto& map = bank.map();
        require(map.layout_warnings().empty(), "unexpected layout warning");
        require(route_calls == 0 && index_calls == 0, "finish must not call resolvers");
        // DOC-BEGIN access
        // VF3 的业务入口与 PF 的 VF3 槽位指向同一份 vf[3]。
        require(map.mmio_write(vf_window(3) + HEAD_SIZE, 0xa3).success(), "VF write");
        require(bank->vf[3].CONTROL.ref() == 0xa3, "VF entity");
        require(map.mmio_read(pf_slot(3) + HEAD_SIZE) == 0xa3, "PF slot shares VF storage");

        // 所有 VF 的 +0x40 都指向 PF 的同一个 CPU_SHARED。
        require(map.mmio_write(vf_window(5) + HEAD_SIZE + 0x40, 0x77).success(), "shared write");
        require(bank->pf.CPU_SHARED.ref() == 0x77, "shared entity");
        require(map.mmio_read(pf_slot(0) + HEAD_SIZE + 0x40) == 0x77, "shared PF slot");
        require(map.mmio_read(vf_window(7) + HEAD_SIZE + 0x40) == 0x77, "shared VF window");

        // 动态身份：请求 VF2 -> vf[6]；两个入口使用同一 resolver key。
        route[2] = 6;
        const auto before = route_calls;
        require(map.mmio_write(vf_window(2) + HEAD_SIZE, 0x66).success(), "redirect write");
        require(bank->vf[6].CONTROL.ref() == 0x66 && bank->vf[2].CONTROL.ref() == 0, "redirect target");
        require(route_calls == before + 1, "resolve identity once");
        require(map.mmio_read(pf_slot(2) + HEAD_SIZE) == 0x66, "same redirect from PF slot");

        // QUEUE[1][2] -> vf[6].QUEUE[1][3]；身份转换先于数组下标转换。
        require(map.mmio_write(vf_window(2) + HEAD_SIZE + 0x128, 0x55).success(), "queue write");
        require(bank->vf[6].QUEUE.at(1, 3).ref() == 0x55 && index_calls == 1, "mapped queue");
        // DOC-END access

        // 只读模块、模型读值、64 bit transaction 与显式 8 字节写。
        require(map.mmio_write(vf_window(0) + 8, 99).result == reg::RegMemTxResult::Ignored, "RO window");
        require(bank->module.STATUS.ref() == 0, "RO window preserved storage");
        require(map.mmio_write(0, 99).result == reg::RegMemTxResult::Ignored, "RO register");
        require(map.mmio_read(vf_window(3) + HEAD_SIZE + 8) == 777, "model read");
        require(bank->vf[3].STATUS.ref() == 0, "model read preserved storage");
        require(map.mmio_read(0x20) == 0x55667788 && map.mmio_read(0x24) == 0x11223344, "64-bit transaction snapshot");
        require(map.mmio_write_sized(HEAD_SIZE + 0xc0, 0x123456789abcdef0ull, 8).success(), "64-bit write");
        require(bank->pf.DESC_BASE.ref() == 0x123456789abcdef0ull, "64-bit value");

        require(map.mmio_write(MiB + 0x10, 1).success(), "PF command");
        require(map.mmio_write(vf_window(2) + MiB + 0x10, 1).success(), "VF command");
        require(command_calls == 2 && last_request == 2 && last_resolved == 6 && last_command_offset == 0x10,
                "command identity and offset");
        require(map.try_mmio_read_sized(vf_window(0) + MiB, 4).result == reg::RegMemTxResult::DecodeError,
                "undeclared command address");
        for (auto address : {pf_slot(0), 10 * MiB, vf_window(0) + HEAD_SIZE + 0x10})
            require(map.try_mmio_read_sized(address, 4).result == reg::RegMemTxResult::DecodeError, "unmapped hole");

        // DOC-BEGIN state
        const auto saved = map.dump_state_versioned();
        map.reset_matching({reg::ResetRequest::VfFlr, 6});
        require(bank->vf[6].CONTROL.ref() == 0 && bank->vf[3].CONTROL.ref() == 0xa3, "VF FLR isolation");
        require(bank->pf.CPU_SHARED.ref() == 0x77, "shared PF entity survives VF FLR");
        map.restore_state_versioned(saved);
        require(bank->vf[6].CONTROL.ref() == 0x66, "snapshot restore");
        // DOC-END state
        std::cout << "PASS: PF/VF shared storage, identity/index routing, RO, callbacks, commands, reset and snapshots\n";
    }
};
} // namespace example

int main(int argc, char** argv) try {
    using namespace example;
    Device device;
    auto& map = device.bank.map();
    const std::string_view option = argc > 1 ? argv[1] : "";
    if (argc == 1) device.demonstrate();
    else if (argc != 2) throw std::runtime_error("expected a single option");
    else if (option == "--layout") std::cout << map.describe_layout();
    else if (option == "--layout-full") std::cout << map.describe_layout(reg::LayoutDetail::Full);
    else if (option == "--explain") {
        // DOC-BEGIN explain
        // VF2 的 QUEUE[1][2]；只解释规则，不调用设备路由函数。
        const auto address = vf_window(2) + HEAD_SIZE + 0x128;
        std::cout << map.explain_address(address);
        // 假设 resolver 返回 6，预览对应实体；数组映射仍以符号规则显示。
        std::cout << map.explain_address(address, 4, reg::RegTransactionKind::Read, 6);
        // DOC-END explain
    } else throw std::runtime_error("usage: regfw_pf_vf [--layout|--layout-full|--explain]");
    if (argc > 1) require(device.route_calls == 0 && device.index_calls == 0, "inspection called a resolver");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
```

把 CPU/GROUP 拆成八个共享实体组时，每组单次 REG_ENTITY，再组合到一个 shared View；所有 VF 业务 View 通过 map(shared,0,10) 复用。偏移只加一次，实际实体覆盖会 warning。

## 13. 命令、doorbell、FIFO 全部是实体

0.23.0 删除无实体 command 区域。每个有效接口都用 REG 定义；同样享有类型、复位、快照、数组、权限、回调、transaction 和日志，没有第二套 CommandPort 回调签名。

```cpp
// 片段：一个普通 doorbell 和一个 512 bit 命令端口。
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(DOORBELL, std::uint32_t, reg::at(0).access(reg::write_only));
    REG(PACKET, reg::u512,
        reg::at(0x40).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
};
```

REG 始终有 backing storage，即使它只是门铃或读写不同含义的端口。普通 32 bit doorbell 每次合法写后执行 on_write；512 bit PACKET 在完成端提交后执行一次。动态读返回完整响应，不覆盖写实体，读完成副作用用 on_read_complete。详情见 §7～§9。

无 on_read 时返回 behavior 的候选值；无 on_write 时只执行寄存器自身 behavior，不再沿用旧 command“缺回调 AccessError”的行为。write_only 的默认读取是 0，仍可绑定 on_read 返回设备响应。未声明的命令地址是 DecodeError；不要为了覆盖一个大 aperture 而随便造大量不存在的命令。

PF/VF 命令实体归属和其他 REG 一样：全局一个实体用 REG_ENTITY，每 VF 独立用 REG_REPEAT_ENTITY。通过 View.map 映射地址；同一实体多个入口仍共享值和事务，bank.save 会统一处理 Migratable 实体。

## 14. 回调 info 与 MMIO 执行顺序

### 14.1 RegAccessInfo 字段

| 字段/方法 | 含义和注意事项 |
|---|---|
| `kind`、`is_read()` / `is_write()` | Read / Write |
| `address` | 软件原始 map 内地址，即日志 bus |
| `alias_resolved_address` | 原始入口的静态映射结果，即 alias |
| `resolved_address` | 身份路由之后的地址；下标转换不重编码它 |
| `size` | 本拍访问字节数 1/2/4/8 |
| `internal_byte_offset` | 本拍在完整寄存器值内的字节偏移 |
| `raw_data` | 本拍原始写数据，按 size 截断；读请求为 0 |
| `bus_data` / `bus_mask` | 对齐到完整寄存器内部位置的数据/掩码；事务写完成时表示聚合写入 |
| `stored_before` / `stored_after` | 本次行为前后存储值；原始 RegAccessInfo 只有低 64 bit |
| `register_width_bits` / `is_wide()` | 完整实体值位宽 / 是否超过 64 bit |
| `wide()` | 原始 info 的完整 WideAccessValues；不存在时抛异常 |
| `request_index` / `index` | 下标转换前/后的完整数组元素索引 |
| `index_fields` / `mapped_fields` | 原始命名坐标 / 派生字段 |
| `index_as(key)` / `mapped_as(key)` | 按类型化 key 取原始/派生字段 |
| `request_id()` / `resolved_id()` | 原始 / 有效 VF 身份；缺少身份时抛异常 |
| `has_identity()` / `is_identity_domain(d)` | 是否有身份 / 是否属于指定域 |
| `source_instance` / `request_id_or_instance()` | 来源重复实例 / 优先返回请求身份，否则返回来源实例 |
| `route` | 方向标记 kind 和来源 instance，不是有效 VF 编号 |
| `is_alias` / `alias_access_policy` | 是否使用静态映射 / 入口权限 |
| `access_group` | 访问组名；不代表实体的新名称 |

通过 REG 泛型回调拿到的是类型化 info：`info.index.get()` 返回整数/tuple；宽寄存器的 `info.raw_data/bus_data/bus_mask/stored_before/stored_after` 被宽值引用遮蔽，代表完整值。要统一处理原始字段可使用 `info.raw`；显式声明 `const reg::RegAccessInfo&` 时使用 `info.wide().stored_after` 取得宽值。不要混淆两种 info 类型的字段宽度。

无论哪种 info，`on_read_complete` 的响应内容优先用传入 T snapshot，stored_after 表示存储值而非响应快照。不要保存 info 的指针、引用或 TypedRegElementIndex 到回调之外；需要异步使用时复制地址、整数坐标和实际值。

### 14.2 正常执行顺序

正常 REG 写：入口映射/identity → 权限和宽度检查 → access-group 下标解析 → pre_access → behavior/事务暂存或提交 → 更新实体 → 日志 → on_write。暂存拍不会执行最终 on_write；其他阶段如 identity/index/pre_access 仍会逐拍执行。

正常 REG 读：入口映射/identity → 权限和宽度检查 → access-group 下标解析 → pre_access → behavior/建立或取用快照 → on_read（只在策略要求时）→ 取本拍响应 → 日志 → on_read_complete（完成时）。

例外：内建纯 RO 寄存器的写会校验访问宽度，然后直接 Ignored，跳过下标 resolver、pre_access、事务暂存、behavior 和 on_write。identity 仍可能先执行以确定实体；入口权限拒绝也可能更早返回。

`map.set_pre_access(f)` 的全局 f 接收 `const RegAccessInfo&`，用于统一访问前检查/观测，不返回新寄存器值；不是异步调度点。当前异常语义：pre_access 抛异常会打印；写返回 Ignored，读返回 callback-error 值 0、状态可为 Ok。要返回明确访问错误，优先使用解析/验证阶段的错误机制，不能把任意 pre_access 异常当成总线 fault。

## 15. MMIO API、返回状态与异常

### 15.1 调用形式

| API | 返回 | 默认访问宽度 |
|---|---|---|
| `map.mmio_write(addr,value)` | RegMmioWriteResult | 固定 4 字节 |
| `map.mmio_read(addr)` | RegData，即 uint64_t；只取数值 | 固定 4 字节 |
| `map.mmio_write_sized(addr,value,size)` | RegMmioWriteResult | 必须显式给 size |
| `map.mmio_read_sized(addr,size)` | RegData；不暴露状态 | 必须显式给 size |
| `map.try_mmio_write_sized(addr,value,size)` | RegMmioWriteResult | 同 write_sized |
| `map.try_mmio_read_sized(addr,size)` | RegMmioReadResult，含 value 和 result | 同 read_sized |

需要判断错误时使用 try_mmio_read_sized，不要只拿 mmio_read 的数值。write 接口本来就返回状态。写值超过 size 的位宽会可选 Warn 并截断高位；不会根据 value 自动升级为 8 字节访问。

AccessWidthPolicy：`LEGACY`、`U8_ONLY`、`U16_ONLY`、`U32_ONLY`、`U64_ONLY`；也可用 `only(n)`、`sizes_1_2_4()`、`sizes_4_8()`。默认 `RegisterAlignment::Natural`；`.with_alignment(RegisterAlignment::RegisterBase)` 只允许从实体起始偏移访问，不适合需要高端拍的宽事务。允许集合不代表跳过实体范围、实际对齐和 transaction 协议检查。

### 15.2 结果矩阵

| 情况 | 状态 / 行为 |
|---|---|
| 正常 | Ok |
| 寄存器纯 RO 写、入口只读忽略写 | Ignored，不执行写业务回调 |
| 入口只读错误写、访问权限拒绝 | AccessError |
| 保留区、未声明寄存器空洞 | DecodeError |
| 不支持宽度/对齐、宽事务无合法首拍 | Unsupported |
| 解析器缺失或普通异常 | DeviceError |
| 身份越界/显式身份拒绝 | 默认 AccessError；配置 identity Ignore 时为 Ignored |
| REG on_read 抛异常 | 打印；返回 REG_READ_CALLBACK_ERROR_MAGIC（当前为 0），状态可为 Ok |
| REG on_write 抛异常 | 打印；写仍可为 Ok，不回滚已执行的修改 |
| on_read_complete 抛异常 | 打印；已经生成的本拍响应仍返回 |

`.success()` 对 Ok 和 Ignored 都返回 true；不是“实体一定被修改”的判据。`INVALID_READ_VALUE` 是 `0xDEADBEEFDEADBEEF` 按 size 截断；它与读回调异常时的 0 不同。数值都可能与合法设备值重合，不能作为唯一错误检测方式。

日志仅在绑定 logger 且相应等级启用时输出。`std::exception` 打印 what()；`reg::Error` 包含抛出位置及可选原因链/register dump；未知异常打印 non-standard C++ exception。`reg::context("...",lambda)` 可包装并保留异常原因链。只能捕获 C++ 异常，不处理 SIGSEGV、abort、assert 终止，也不能捕获越过 noexcept 导致的 terminate。

## 16. 复位与快照

### 16.1 复位

scope 与 domains 同时匹配才参与选择性复位：

| ResetRequest | 对应 domain | scope 选择 |
|---|---|---|
| PowerOn | POWER_ON | 全部 scope |
| Cold | COLD | 全部 scope |
| Warm | WARM | 全部 scope |
| PfFunction | FUNCTION | Global 和 Pf；不自动包含 Vf |
| VfFlr，带 vf_id | VF_FLR | 仅 Vf 且 vf_id 匹配 |
| Software | SOFTWARE | 全部 scope |

```cpp
// 片段：map 已初始化。
map.reset_matching({reg::ResetRequest::VfFlr, 3});
map.reset_matching({reg::ResetRequest::PfFunction});
// 不考虑 scope/domains，全部实体复位：
map.reset();
```

`ResetDomains::NONE` 排除所有选择性域；STANDARD/ALL 包含 6 个标准域。实体组配置为默认 scope，某个 REG 可显式 `.reset_scope(...)` 覆盖。

`REG_REPEAT_ENTITY(...,reg::entity().reset_per_instance(reg::ResetScope::Vf))` 自动给第 i 组 Vf(i)。单实体不得用 reset_per_instance。窗口 `.identity(...)` 不是复位配置；在 VF window 中 map(PF实体) 不会把实体变为 Vf scope。

`map.reset()` / `bank.reset()` / bundle.reset() / RegView.reset() 的作用范围不同：map 全部，bank 根 bundle 全部，bundle 对应存储子树，RegView 为整个实体。复位写入 reset 值并清除 transaction 状态，不调用 MMIO 读写回调，不自动清业务 FIFO/设备线程状态。

### 16.2 完整快照（v2）与 QEMU 模块流

```cpp
// 完整快照：值 + 每元素进行中的分拍状态。
auto saved = bank.save();
bank.load(saved);
// 与 map.dump_state_versioned()/restore_state_versioned() 等价。
```

保存 Migratable 实体值、每元素统一事务的 write_pending/write_mask/read_snapshot/has_read_snapshot/read_source_ok。共享实体只保存一次；数组元素独立记录，空闲状态不展开大块 payload。Transient/Derived 的值与暂存均排除且 load 不触碰。

保存半条命令后，恢复并继续写剩余拍可正确提交；保存读快照后，恢复继续读剩余拍不重新调用 on_read，并按保存的成功标记处理 on_read_complete。load 不重放业务回调，实体原生引用仍有效。

schema v2 记录 stable_id、order、word 布局、所有位宽、数组 dimensions/strides、transaction 策略和哈希。恢复先验证 schema、索引、payload 位宽/掩码及完整性，再准备所有实体值/暂存并统一提交；错误不改变 bank。stable_id 和显式 order 需全 map 唯一。schema 不替代设备字段语义的版本管理，v1 格式明确拒绝。

**沿用已有 vmstate_fields，不用 VMStateDescription**：包含 `<regfw/vmstate.hpp>` 后可 `bank.save(file)` / `bank.load(file)`，另有大写 Save/Load 别名。该可选头使用工程已有的 `<vmstate_fields.hpp>` 和 qemu_interface.h，底层还是 module_var_save/load。`extras/vmstate_fields.hpp` 附带此前版本的原样副本供复用。

父对象使用 `VMSTATE_FIELDS(bank, fifo, vf_route, ...)` 即可递归登记 bank；bank 已有 VmstateFields hook，不需列出其私有字段。此形式只要求原有 vmstate_fields 与 regfw 公共头。也可自行构造带 Limits 的 Writer/Reader 并调用 bank.save(writer)/load(reader)。流协议继承原有框架的 Host 原生表示、容器数量/总字节/递归限制；底层 module_var_load 必须完整读取或报告失败。

不保存函数、指针、logger、锁、设备 FIFO、线程、resolver 捕获的可变路由表；由设备初始化和父字段清单管理。目标先初始化同布局 bank 和回调。整个父对象的 Load 不自动事务化，但 bank 自身校验失败时不会半恢复。

**兼容接口区别**：`dump_state(span)` / `restore_state(span)` 仍是裸值 word 数组，无法保存分拍状态，恢复会清空参与恢复实体的暂存。完整保存优先使用 bank.save。≤64 bit 每元素一个 word，宽值占 ceil(bits/64) word；别把字节数、word 数和元素数混用。

保存/加载需与 MMIO/设备线程同步；允许停在两次 guest MMIO 之间，不允许在同 map 的 MMIO 回调中递归保存/加载。流格式、C/C++ 异常边界、接入示例和测试范围详见 [Snapshot.md](Snapshot.md)。

## 17. 并发、事务交错、重入与异步

| 情况 | 结论 |
|---|---|
| 同一元素一笔读和一笔写，分拍交错但各 MMIO 串行进入 | 支持：read_snapshot 和 write_pending 独立 |
| 同一元素两笔写命令交错 | 不隔离：共用一份 pending/mask，可能混成错误命令 |
| 同一元素两笔读命令交错 | 不隔离：第二次首拍会替换当前快照 |
| 不同地址入口映射同一实体元素 | 仍共享事务状态，不因入口不同而隔离 |
| 不同数组元素/不同 VF 实体 | 分别有元素事务状态，但同一 map 的调用仍需同步 |
| Host 多线程同时操作同一 RegisterMap | 不支持自动线程安全，必须外部串行化 |
| on_read/on_write 回调里递归调用同一 map 的 MMIO | 不支持，DispatchGuard 拒绝；不是支持重入的递归锁 |
| 同步回调内直接读取/修改普通实体 `.ref()` | 可作为内部访问；不要在 behavior borrow 阶段重入同一实体 |
| 回调把命令提交给异步工作线程 | 设备自行实现队列；复制值和必要上下文，不保存 info/实体可变引用 |

map 的 dispatching_、entity borrowed 都是普通重入状态，不是 atomic 或 mutex；不要把异常检测当成并发保护。QEMU 工程若已有覆盖该路径的锁可复用，但不能仅因使用了 QEMU/MMIO 就假定所有访问及异步设备线程都被保护。

逐次 MMIO 加锁只能保护 Host 数据竞争，不能阻止两个 guest CPU 的两条宽命令按拍混叠。完整命令的独占所有权要由 guest 软件协议、不同通道/实体，或额外的设备发起者事务机制保证。本版不按 vCPU/线程/reqid 自动给同一实体复制事务上下文。

FIFO 生产者/消费者和外部模型数据也必须同步；仅锁住 map 不自动保护设备工作线程。reset、快照、换绑 resolver 和内部 .ref() 更新也应纳入设备的一致性规则。

## 18. 日志、布局检查、地址解释与值 dump

### 18.1 hlog 接入和等级

用 reg::space(...).logger(logger) 或 map.set_logger(logger) 绑定现有 HLogLogger* / hlog::Logger。logger 必须比 bank 活得更久。未绑定或等级被过滤时不输出；错误捕获仍执行。

Debug 每次访问一行摘要；Trace 也为单行，使用 ` | ` 分隔操作结果、地址链、身份/下标。框架不再把完整 C++ 函数签名塞进 hlog 前缀；不改变设备自己写的其他 hlog 日志。

```text
WRITE POE_GRP_VCPU_MASK[1][0]: 0x00000000 -> 0x00000030 | addr: 0x01225020 -> 0x00325020 | size=4B | write=0x00000030 | mask=0xffffffff | route=VF_TO_PF | vf=1 | domain=1 | group=VF_VCPU_TO_LCPU
```

| 日志规则 | 含义 |
|---|---|
| READ REG -> value | 显示本次响应片段，不误用实体值 |
| WRITE REG: before -> after | 显示实体实际变更；暂存拍可保持不变 |
| txn=stage-write / commit-write | 事务暂存/提交 |
| txn=snapshot-read / snapshot-lane / complete-read | 建快照/中间拍/完成拍 |
| route=VF_TO_PF 等名称 | 不再输出 256/257 枚举数字 |
| vf=1；或 vf=1->6 | 身份未变只显示一次，变化时显示映射；非 VF 方向使用 id= |
| index=[1][0]->[3][0] | 只在下标实际变化时附加转换前后多维坐标；不输出展平 request_index/index |
| 地址 A -> B | 原始地址到最终地址；中间 alias 只有与两端都不同时才额外显示 |
| mask / data | 显示访问掩码；对齐数据与原始写值相同可省略重复 data |
| group 和命名坐标 | 保留访问组及逻辑/物理字段名和值 |
| ERROR path=完整规范路径 | 错误保留完整定位、有效坐标、前后存储值和异常原因链 |

主行寄存器名省略冗长窗口前缀，数组保留实际多维坐标；addr 和身份可区分入口。规范路径不是请求来源；身份有效编号看 vf/id，下标转换后地址不重编码。固定布局输出仍使用完整层级。

finish 的 Debug 只输出 View/Entity 大范围，Trace 才输出寄存器地址详情（TRACE 等级）；Summary/Full 对应这两档。数组按维度、stride、公式及可见选择折叠，重复 VF 只列一个代表。Info 默认不打印布局。LogPolicy::Trace 要求 Trace 才打印访问；Hidden 隐藏普通访问日志，不禁用 ERROR。不要调用不存在的 map.set_log_formatter；格式已在框架实现。

### 18.2 静态诊断 API

```cpp
// 片段：map 已构建。
auto summary = map.describe_layout();
auto full = map.describe_layout(reg::LayoutDetail::Full);
auto explanation = map.explain_address(0x100, 4, reg::RegTransactionKind::Read);
auto assumed = map.explain_address(0x100, 4, reg::RegTransactionKind::Read, 6);
auto warnings = map.layout_warnings();
```

describe_layout / explain_address 不执行 resolver、predicate、behavior 或回调，不修改寄存器。给 known identity=6 是“假设 resolver 返回 6”的静态预览，不是真实调用结果。

`resolve_address(addr,size)` 和 `resolve_address_route` 只解释静态映射；`resolve_address_with_identity(addr,size)` **会执行 identity resolver**，返回地址/route/identity，但不执行完整数组下标和读写业务逻辑。不要把它当无副作用的 explain_address。

布局错误包含双方声明位置、区间及名称。`REGFW_MISSING_IDENTITY_DOMAIN` 是条件引用的身份域没有任何声明窗口；fallback 可能有意使用，因此是警告。诊断检查不能代替设备运行协议验证。

### 18.3 当前值 dump

```cpp
// 片段：选一个标量和一个数组。
auto dump = reg::reg_dump(bank->core.CONTROL);
dump.push_all(bank->core.COUNTERS);
auto text = dump.str();
```

`RegDump::push(reg)` 取标量/元素；`push_all(array)` 取全部；`push_range(array,begin,end)` 为半开范围，或传索引集合。读取的是内部存储，不触发 on_read，不能用它查看 FIFO 动态响应。可选 `T::field_descriptors` 或 `reg_dump_fields()` 提供字段解释；没有元数据时仍可打印 raw。`reg::Error(...).with_dump(dump.str())` 可附加错误上下文。

## 19. 完整示例 D：混合位行为、自定义 behavior、96 bit 和快照

此例覆盖非 2 次幂宽度、64 bit transaction、普通回调异常和一维 subspan。它也说明 custom behavior 要用 REG_DYNAMIC，且快照不会调用 MMIO 回调。

<!-- runnable: advanced -->
```cpp
#include <regfw/regfw.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>

struct AdvancedRegs : reg::RegEntityBundle {
    REG_BUNDLE(AdvancedRegs);
    REG(MIXED, std::uint32_t,
        reg::at(0).reset(0x1234ffffu).access(reg::mixed(
            reg::rule(0x000000ffu, reg::rw),
            reg::rule(0x0000ff00u, reg::w1c),
            reg::rule(0xffff0000u, reg::ro))));
    REG_DYNAMIC(CUSTOM, std::uint32_t,
        reg::at(4).access(reg::FieldBehavior{
            [](reg::RegData, reg::RegData value, reg::RegData) {
                return value ^ 0xffu;
            },
            [](reg::RegData& stored, reg::RegData mask) {
                return stored & mask;
            }}));
    REG(WIDE96, reg::WideUInt<96>,
        reg::at(0x20).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(TXN64, std::uint64_t,
        reg::at(0x40).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(ITEMS, std::uint32_t,
        reg::at(0x60).array(reg::dims(6), reg::strides(8)));
    REG(FAIL, std::uint32_t, reg::at(0xa0));
    REG(TEMP, std::uint32_t,
        reg::at(0xa4).snapshot(reg::SnapshotKind::Transient));
};
struct AdvancedLayout : reg::Bundle {
    REG_BUNDLE(AdvancedLayout);
    REG_ENTITY(core, AdvancedRegs);
    REG_LAYOUT([&] {
        return reg::address_space("DEVICE")
            .map(reg::view("CORE", 0x100).map(core), 0);
    });
};
void require(bool ok) {
    if (!ok) throw std::runtime_error("advanced check failed");
}
int main() {
    reg::RegisterBank<AdvancedLayout> bank(reg::space(0x1000));
    unsigned writes = 0, reads = 0, completed = 0;
    bank->core.WIDE96.on_write([&] { ++writes; });
    bank->core.TXN64.on_read([&]() -> std::uint64_t {
        ++reads;
        return 0x1122334455667788ull;
    });
    bank->core.TXN64.on_read_complete(
        [&](std::uint64_t v, const auto&) {
            if (v == 0x1122334455667788ull) ++completed;
        });
    bank->core.FAIL.on_read([]() -> std::uint32_t {
        throw reg::Error("expected read-source failure");
    });
    bank.finish();
    auto& map = bank.map();
    require(map.mmio_write(0, 0xff000055u).success());
    require(bank->core.MIXED.ref() == 0x1234ff55u);
    require(map.mmio_write(4, 0xaau).success());
    require(map.mmio_read(4) == 0x55u);
    require(map.mmio_write_sized(0x20,0x1122334455667788ull,8).success());
    require(writes == 0);
    require(map.mmio_write(0x28,0xaabbccddu).success() && writes == 1);
    require(bank->core.WIDE96.ref().word(0) == 0x1122334455667788ull);
    require(bank->core.WIDE96.ref().word(1) == 0xaabbccddu);
    require(map.mmio_read(0x40) == 0x55667788u);
    require(map.mmio_read(0x44) == 0x11223344u);
    require(reads == 1 && completed == 1);
    auto middle = bank->core.ITEMS.subspan(2,3);
    middle.at_element(0).ref() = 9;
    require(bank->core.ITEMS.at(2).ref() == 9);
    auto failed = map.try_mmio_read_sized(0xa0,4);
    require(failed.success() && failed.value == reg::REG_READ_CALLBACK_ERROR_MAGIC);
    bank->core.TEMP.ref() = 7;
    auto state = map.dump_state_versioned();
    bank->core.CUSTOM.ref() = 0;
    bank->core.TEMP.ref() = 8;
    map.restore_state_versioned(state);
    require(bank->core.CUSTOM.ref() == 0x55 && bank->core.TEMP.ref() == 8);
    auto dump = reg::reg_dump(bank->core.CUSTOM);
    dump.push_all(bank->core.ITEMS);
    require(!dump.str().empty());
    std::cout << "PASS advanced\n";
}
```

## 20. 构建、QEMU 接入与编译开销

### 20.1 推荐 CMake 接入

要求 CMake >=3.20、C++20 编译器；使用发布包时 bundled hlog 位于 SDK 的 third_party/hlog。框架本身只依赖 hlog C 头和库，不要求 fmt。

```cmake
# 工程内目录示例。
add_subdirectory(third_party/regfw)
add_executable(device main.cpp)
target_link_libraries(device PRIVATE regfw::regfw)
```

若 SDK 在当前源目录树之外，明确提供 binary directory：

```cmake
add_subdirectory("${REGFW_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/regfw")
```

现有 hlog 可用 `-DHLog_ROOT=/absolute/path/to/hlog`；该目录包含 include/hlog.h 与 lib/libhlog.so。优先使用大小写精确的 `HLog_ROOT`，避免不同 CMake policy 对 HLOG_ROOT 的警告/忽略。同一进程使用一致的 hlog runtime。已有 HLog::HLog 目标也可复用。

regfw::regfw 传播 C++20、include 路径、GNU/Clang 的 `-fno-strict-aliasing`；手写编译命令不要漏掉原生 union 所需选项。设备自行使用 hlog.hpp 的格式化宏时，再链接所需 fmt；这不是 regfw 核心的额外需求。

SDK 根目录构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREGFW_BUILD_EXAMPLES=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

REGFW_BUILD_EXAMPLES 默认 OFF；REGFW_BUILD_TESTS 顶层构建默认 ON，被 add_subdirectory 引入时默认 OFF。测试打开时需要 Python >=3.9，并会构建所需示例。安装导出的目标仍是 regfw::regfw：消费者 `find_package(regfw CONFIG REQUIRED)`，提供安装前缀和 HLog_ROOT；regfw 安装不自动把 bundled hlog 安装到系统目录。

### 20.2 大型实体组编译

大量 REG 是设备侧模板/声明实例化，PCH 应缓存设备真实声明头，不只框架头。`regfw_precompile_bundle(device,"device_regs.hpp")` 是可用的 CMake 辅助函数（bundle_pch 测试端到端覆盖）。只声明寄存器的设备头可包含 `<regfw/bundle.hpp>` 代替完整入口，使 PCH 缓存的头部不携带 constexpr 布局编译器；定义 REG_LAYOUT_IMPL 的 .cpp 仍用 `<regfw/regfw.hpp>`。

可把构造/析构放到一个 cpp：类内用 `REG_BUNDLE_DECL(MyRegs);`，同一 namespace 的 cpp 用 `REG_BUNDLE_IMPL(MyRegs);`。确保该 cpp 加入设备 target。不要因看到声明头就认为无需链接 src 的编译库。

### 20.3 QEMU C 边界

包含 `<regfw/qemu_bridge.h>`。C++ 使用 `reg::make_qemu_bridge(map)` 得到非拥有的 `regfw_mmio_bridge`：

```cpp
// 片段：bridge 是可按值保存的 C 接口，opaque 指向 map。
auto bridge = reg::make_qemu_bridge(bank.map());
std::uint64_t value = 0;
auto read_result = bridge.read(bridge.opaque, 0x100, &value, 4);
auto write_result = bridge.write(bridge.opaque, 0x100, 42, 4);
```

bridge 类型含 opaque/read/write；bank/map 必须比它活得更久。返回 QEMU MemTxResult 位：Ok/Ignored→0，DecodeError→2，AccessError→4，其余→1。设备自行把它接到 read_with_attrs/write_with_attrs，框架头不依赖具体 QEMU 版本。

如果 QEMU 回调提供子窗口内 offset，先加一次该窗口在 map 内的基址。例如 VF i：`16 MiB + i*2 MiB + off`。不要加 guest 物理 BAR 基址，不要对已经是全局 map 偏移的地址再加一次。map 的访问宽度策略与 QEMU MemoryRegion 的 valid/impl 访问宽度需要相互匹配。

C bridge 不提供锁、发起 vCPU ID、设备时钟、DMA、IRQ 或事务调度；这些由设备工程接入。不要期待多核 CPU 自动得到各自的宽事务上下文。

## 21. 常见错误：症状 → 原因 → 正确写法

| 症状/错误想法 | 原因 | 正确处理 |
|---|---|---|
| PF 和 VF 相同寄存器值不共享 | 重复声明了实体 | 单次 REG_ENTITY，多入口 map |
| PF scope 必须有 PF 地址 | scope 和地址混淆 | scope 只管选择性复位，无 PF 入口也可为 Pf |
| `.vf_to_pf()` 应该自动选 PF 实体 | 把方向标记当目标 | 用 map(target) 明确目标 |
| 修改 `vf[i].REG.ref()` 没触发回调 | 内部访问绕过 MMIO | 想模拟软件访问时调用 map.mmio_* |
| 每个数组元素注册一个 on_write 失败 | 回调槽属于整个实体 | 在数组根绑定一次，按 info.index 分派 |
| on_write(value,info) 的 value 是旧值 | 这是本版契约 | 新值用 stored_after 或同步读取 ref |
| 512 bit on_read 只返回 uint32 | 返回的片段没有包含完整响应 | 返回完整 512 bit T / optional<T> |
| 用 info.stored_after 当 FIFO 读响应 | stored_after 是实体存储 | 用 on_read_complete 的 T snapshot |
| 把一条命令声明成 16 个 uint32 REG | 每个元素独立事务 | 改成一个 512 bit REG，配置 transaction |
| 只写 512 bit 最后一拍也触发提交 | 完成端是触发条件 | 软件保证协议完整性；框架不验证覆盖 |
| REG_ARRAY 的 16 个 uint32 没有整体完成回调 | 16 个元素是独立事务 | 用一个带 u32[16] 的 512 bit 值 |
| 64 bit 什么都不配却期望自动快照 | 默认 INDEPENDENT | 显式 transaction LOW_TO_HIGH |
| 只配置写侧 CommitOnHigh，读仍多次触发 | 读模式仍是 Independent | 需要快照时把读侧设置为 SnapshotOnLow，或选 LOW_TO_HIGH |
| 两笔写命令串起来数据混乱 | 同元素共用 pending | 软件/通道保证整笔事务所有权 |
| 仅加每拍锁，仍有两 CPU 命令混叠 | 锁数据竞争不等于隔离整笔协议 | 独立通道、软件事务锁或设备额外上下文 |
| 多次调用 on_read 想换绑 | 普通回调只可注册一次 | 回调捕获可变模型状态；resolver 另有换绑 API |
| `.map(vf[i])` 的共享子窗报冲突 | 子窗覆盖父实体声明 | 父实体在整个子窗范围内留空 |
| target 地址没体现 pool 下标变化 | 框架不重编码下标后的地址 | 看 info.index / mapped_as |
| 内部 .address() 显示 PF_VF 路径 | 首次映射提供规范入口 | 本次请求看 info.address，不从规范路径推请求源 |
| `.success()` 为 true，但读回调失败 | 当前读异常返回 0/Ok | 看 ERROR 日志及设备业务状态，不靠 magic 值唯一判断 |
| callback 抛异常却没有日志 | 无 logger 或 ERROR 被关闭 | 绑定有效 hlog logger 并启用等级 |
| reset 后 FIFO 没清空 | FIFO 不归寄存器框架持有 | 在设备 reset 同步处理业务状态 |
| 裸 span 恢复后半条命令丢失 | 旧值数组接口不包含暂存 | 改用 bank.save/load 或版本化 v2；业务状态父对象登记 |
| REG 使用 lambda/behavior 工厂编译失败 | spec 不是 constexpr | REG_DYNAMIC，或把设备逻辑放绑定回调 |
| 找不到 inc/dec、bit-range helper | 混用了其他版本/Rust API | 用本版 ref/raw union/WideUInt，或设备自写函数 |

## 22. 兼容接口、低级接口及维护边界

删除项不属于兼容接口：Split64Policy / split64 builder，以及 CommandPort / command window / REG_COMMAND_REGION。旧代码需要按 Migration.md 迁移。

旧 Window DSL 已删除。新模型使用 AddressView / AddressSpace；旧代码可维护这些兼容接口：

| 接口族 | 当前定位 |
|---|---|
| `REG_APERTURE` / `REG_REPEAT_APERTURE` | 在声明时同时分配地址和存储 |
| 带 `reg::aperture().at(...).size_bytes(...).stride(...)` 的实体声明 | 兼容 aperture 模式 |
| `REG_VIEW` / `REG_REPEAT_VIEW` + RegViewBundle | 旧式地址 view 容器；不能创建 REG 实体 |
| `REG_GROUP` / `REG_REPEAT_GROUP` | 旧式有地址嵌套组 |
| `REG_LAYOUT(.views=...,.aliases=...,.identities=...,.reserved=...)` | 旧式并列表布局 |
| `reg::view(source).to(target)` / `reg::alias(source).to(target)` | 旧式映射构建器 |
| `RegLayout` / `RegScope` | 低级/旧式构建入口 |
| `reg::init_bank(unique_ptr<RegisterMap>&,unique_ptr<Root>&,spec)` | 管理既有两个指针成员；两者初始必须为空 |
| `map.create_reg<T>(address,name,spec)` | 直接创建有地址的单 REG，用于小测试/底层适配 |

不要在同一根 REG_LAYOUT 中同时使用递归 root 和旧式并列表；会拒绝。不要给本来无地址的实体又套一层已经分配地址的旧 aperture 并再次手算同一基址。低级 create_reg 的 address 是直接 map 地址，spec 的 at 偏移不应再被当成需要叠加的另一层布局；推荐使用 reg::at(0) 的属性 spec。

如果必须维护旧式 identity 的 `IdentityRemapMode::IdentityOnly`，它只记录身份元数据，不像View 的 RedirectBank 那样重新选第 j 个地址窗口。新设计不要从旧模式推断 window 行为。

### 静态地址描述符

REG 宏同时生成 `NAME_spec` 和 `NAME_address`。例如 §3 中的 `CoreRegs::COUNTERS_address.offset(2)` 为 0x28，是**实体组内偏移**；加映射窗口基址 0x100 后才是 0x128。标量描述符可用 `CoreRegs::CONTROL_address.offset(0)`。它们不执行动态 identity/index 解析，也不是该实体所有入口的唯一地址。

`reg::PhysicalAddressWindow{base,size}.addr(descriptor,indices...)` 组合静态窗口与寄存器偏移；`reg::PhysicalRepeatAddressWindow{base,count,stride,size}.instance(i)` 取实例窗口，`.addr(i,descriptor,indices...)` 组合重复实例地址。只把它们用于已知静态布局，不用它们替代 MMIO 的实际路由。

### 文档验证记录

四个 runnable 程序是文档中的回归样例；完整快照另由 tests/snapshot.cpp 验证原有 vmstate_fields/module_var_* 字段流与分拍续传。具体本次构建/测试记录见 SDK 的 VALIDATION.md；真实 QEMU 设备 savevm/loadvm 和 Host 多线程安全不由内存后端测试代替。

### 文档维护契约

- 新增或修改 API 时同步更新：§4 默认表、§7 回调契约、§8 分拍策略、§15 结果矩阵、§17 并发规则和相关完整示例。
- 本文 `<!-- runnable: NAME -->` 后的 cpp 块是完整程序；其他块明确标为片段/接口示意，不保证独立编译。
- 对完整程序进行实际编译运行，而不只检查是否看起来像 C++。
- 文档验证只保证声明范围内的示例/契约；真实 QEMU 的锁、MemoryRegion 拆拍、guest 顺序、设备 FIFO 协议和迁移仍由设备工程验证。
- 未在本文中的特殊需求，先判断是否是组合现有接口；确需扩展时明确标记为新增需求，不要假装框架已有。

紧凑寄存器地址表按每行代表的最小实际可见入口地址进行 64 位数值升序排序；数组仍只列一行。部分覆盖时不使用可能已隐藏的数组首地址作为排序依据。

PF 自己的业务存储应使用独立的 `REG_ENTITY(pf_business, PoeVfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}))`；VF 用 `REG_REPEAT_ENTITY(vf, PoeVfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf))`。不要为 PF 在 VF 重复数组后额外加一项：若 count 为 VF_COUNT+1，vf[VF_COUNT] 确实是独立实体，并会被标为 Vf(VF_COUNT)，打印它不表示硬件新增了一个 VF。PF 业务实体有独立 PF 入口，不能当作 VF 重复族的副本省略。

布局表中的地址映射行统一显示 `VIEW`；映射实体时显示 `VIEW → entity: <原始实体组或 REG 名称>`，`reset` 表示目标存储的复位归属。PF、PF_VF、VF 的多个入口会显示同一个共享实体目标名，不再把每个入口标成新 ENTITY。折叠重复族的目标名/复位信息取 i=j=k=0 代表实例；实体范围列是映射边界，不表示整段地址都有实体寄存器。

## 按 AddressView 绑定 pre_access（0.24.0）

优先使用命名键，布局只声明挂载范围，在 bank 初始化后绑定设备函数：

```cpp
REG_PRE_ACCESS(CheckBusiness); // 放在命名空间作用域

// REG_LAYOUT 中的片段，vf[i] 为此前声明的实体实例：
auto business = reg::view("BUSINESS", POE_BUSINESS_SIZE)
    .map(vf[i])
    .using_pre_access(CheckBusiness);

// bank 初始化后、finish 前：
reg_bank.map().set_pre_access(CheckBusiness,
    [this](const reg::RegAccessInfo& info) {
        // 此处做设备检查；info.address 是原请求地址。
        // info.view_path 是 identity 转换后实际选中目标的 View 路径。
    });
```

独立的小函数也可直接 `.pre_access(fn)`，fn 签名为 `void(const RegAccessInfo&)`。builder 返回新对象，必须使用返回值；同一 View 只能声明一个钩子。不要在 REG_LAYOUT 中捕获局部变量的引用，或捕获可能随 Bundle 构造返回而移动的 this；设备函数推荐使用命名键，等 bank 构建完成后绑定。重复工厂里的 i 如需直接捕获必须按值。

钩子执行顺序为 **全局 map.set_pre_access(fn) → 最终选中路径的父 View → 子 View**，发生在 identity、权限/宽度和 access-group 下标解析后、behavior/事务更新前。动态 VF i→j 时执行 j 的实际目标路径，保留来源 info.address/request_id，info.index/resolved_id 已是转换结果。只执行获胜分支；透明空洞落到另一分支时不会执行空洞 View 的钩子；共享实体在不同入口可使用不同检查。

每次有效 MMIO beat 都调用，包括宽 REG 的暂存写和快照续读。RO 忽略写、入口权限拒绝、未映射/保留地址及 resolver 失败不会运行钩子。内部 ref()/data 更新、reset、save/load、describe_layout、explain_address 不运行钩子。

命名键未绑定时 finish 报 Configuration；有意延迟绑定可 map.defer_pre_access(key)。之后可 set/clear/has_pre_access(key)；clear 只清该键，不影响全局钩子或其他键。已声明但未绑定的 View 钩子在访问时按钩子失败处理，不会静默放行；需要禁用时显式绑定空函数。重绑/clear 与 MMIO 由设备同步，不新增线程安全。

异常沿用原有 pre_access 行为：打印并停止后续钩子、behavior 和寄存器回调；写返回 Ignored，读返回 0（状态可能为 Ok），不回退其他映射。不要用此异常机制要求明确的总线 AccessError；此类需求仍在解析/验证阶段处理。钩子是 void，不使用返回 false 表达拒绝。

钩子列表在构建时预编译，无钩子的记录为空指针；MMIO 不按 View 数量扫描。函数、捕获对象和钩子链不保存到 bank 快照；恢复前按设备初始化流程重建布局并绑定，外部业务状态由父对象保存。

0.24.1 优化了钩子调用：普通 MMIO 调用不复制函数的 shared_ptr；仅当回调在 pre_access 链执行期间重绑/清除时，保留旧函数至本次 pre_access 链结束。回调内自修改仍受支持，后续钩子读取最新绑定；旧捕获对象的析构时点可能延后到本次 pre_access 链结束。框架仍不支持未同步的并发重绑。

## 0.26.0 MMIO 执行计划与回调适配

公开 Entity/View、resolver、pre_access、回调和 transaction 写法保持。finish 把稀疏页目录冻结为连续探测表，并为已映射地址生成 32 字节分发记录；原生 32-bit RW 的数据地址、掩码和静态允许方向已确定，不再逐拍通过冷元数据计算存储地址。大地址间的空洞仍不分配页。

原生 32-bit 无 identity/index 派生或 pre_access、方向 Independent 且没有访问日志时，回调使用轻量调用帧。完整 info 在与用户函数一起编译的适配器中准备，让编译器删除该函数未使用的字段准备；需要完整字段、按值复制 info 或 typed index 的回调仍得到全部语义。读回调的两条适配路径共享同一个可变函数对象，不能各自复制一份计数或 FIFO 状态。异常和回调内启用日志时仍能构造完整诊断信息。

常见的单 uint32 坐标、无条件命名 index 映射减少字段查找；仍逐拍调用当前 resolver，并执行原有维度范围、映射冲突和容量检查。其他条件、多个字段、自定义字段转换、部分访问、RC/W1C、自定义 codec/behavior、宽寄存器和事务保留完整路径。

晚绑定回调、resolver 换绑/清除、全局/View pre_access 和动态日志等级仍生效。on_read 返回值不写回实体，nullopt 使用回调前候选值；读完成顺序、异常和快照 v2 不变。回调 info 只在同步调用期间借用，值复制保留有效字段；名称/View 路径仍借用，禁止 memcpy 整个 info。每 Map 仍需外部同步，不允许同 Map MMIO 重入。

本版以额外的紧凑分发记录和冻结页索引换取访问速度；读源保存一份函数状态和两条调用适配器，初始化开销增加。分发表、函数和缓存指针不进入快照。头文件和库必须一起升级并重新编译。数据、额外内存说明和测试范围见 [MMIO 性能报告](Performance.md)。


## 0.27.0 固定布局入口

固定布局、自动成员描述、layout.vf[i] 及 REG_STATIC_INDEX 的准确用法见 [StaticLayout](StaticLayout.md)。REG_LAYOUT 的 [](auto layout) 入口编译期生成计划；原 [&]{...} 入口仍为动态构建，不会自动转换。初始化默认遵循项目优化等级，REGFW_FAST_COMPILE_INIT 可恢复旧编译策略。功能/快照约束沿用本指南。

## 当前日志与编译优化行为

最终日志文本只在对应等级启用时格式化；0.32.0 的映射快路径还会在构造完整访问上下文之前检查访问日志等级。日志等级可在运行时、resolver 或 on_read 内修改，本次访问仍按原调用顺序执行。读写回调、pre_access、身份/下标解析器接收的字段和值保持原契约。

静态规则数组按块计数只是编译器优化；地址空洞、覆盖、保留区和运行期分发没有新限制。实际输出仍逐 word 验证。初始化直接绑定 word 索引，仍为每个 bank 建立自己的实体和回调绑定，不共享可变状态。
