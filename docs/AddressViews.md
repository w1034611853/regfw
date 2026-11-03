# Entity / AddressView / AddressSpace（0.26.0）

[返回 README](../README.md) · [Agent 完整说明](AgentGuide.md) · [快照](Snapshot.md)

实体只拥有值、回调、事务及复位/快照配置。AddressView 只拥有局部布局描述；一个 View 可组合多个实体和其他 View，也可挂到多个基地址。AddressSpace 将 View 挂载到 64 位地址入口。设备不需要手工展开多层 sub_windows。

旧 `RegView<T>` 仍是某个 REG 实体的类型化句柄；新的 `AddressView` 是一块可组合地址布局，两者职责不同。`REG_VIEW` 旧宏没有改变含义，也不是新 AddressView 的声明方式。

## 精确 API

```cpp
reg::view("NAME", size);                    // AddressView：有名字和局部大小
view.map(entity_group, offset = 0, priority = 0);
view.map(register_or_slice, offset = 0, priority = 0);
view.map(other_view, offset = 0, priority = 0);
view.map_repeat("FAMILY", offset,
    reg::repeat(count).stride(stride), factory, priority = 0);
view.reserve(offset, size, priority = 0);    // 显式终止区，命中后 DecodeError
view.read_only_ignore_writes();
view.read_only_error_writes();
view.vf_to_pf();
view.pf_to_vf();
view.identity(domain).using_resolver(VfRoute);

reg::address_space("NAME");                 // AddressSpace：REG_LAYOUT 的返回值
space.map(view, base, priority = 0);
space.map_repeat("FAMILY", base,
    reg::repeat(count).stride(stride), factory, priority = 0);
```

上述 builder 都返回新对象；链式使用，或循环里赋回变量。`factory(i)` 返回 AddressView，构建时同步执行。重复实例须同形（布局、权限、优先级和身份声明一致），且单实例范围应落在 stride 中。

- entity_group 映射保留组内 REG 的局部偏移。映射组本身不会额外加一遍业务 offset。
- register_or_slice 映射把首元素平移到指定 offset，使用原实体本地布局；不要求事先有地址入口。切片孔洞仍透明。
- other_view 的 offset 相对当前 View；space.map 的 base 相对 AddressSpace。使用完整 guest 物理地址或设备相对地址都可以，但 MMIO 调用必须采用相同坐标。
- View.map_repeat 的 offset 相对当前 View；整个族的 count*stride 必须落在 View.size 内。Space.map_repeat 的 base 相对 AddressSpace。两者都自动建立 repeat 实例族，identity resolver 可将 i 转换为 j。仅循环调用普通 space.map 不会自动建立身份实例族。
- 原有实体类型、REG_ACCESS_GROUP、reset/save/load 与回调绑定方法不变；在 bank 初始化后绑定 resolver，然后 finish。

## 优先级、空洞与 warning

优先级为有符号 int32，默认 0，数值大者优先；只比较同一父容器下的兄弟映射。外层优先级先决定分支，内层的高数值不会穿过父容器影响另一条分支。声明顺序不决定覆盖结果。

**View 是稀疏容器。** 例如共享 View 长 1 MiB，只有 +0x40 和 +0x180 定义了 REG：只在这些解码槽位参与覆盖，其他位置继续使用 VF 背景映射。显式 reserve 是终止区，可以挡住下层实体；纯空洞没有这种作用。

- 只有范围重叠、实际 REG 互补：正常，无 shadow warning。
- 较高优先级实际遮挡另一实体/数组元素：继续构建，保存 `REGFW_ENTITY_SHADOWED` warning。
- 同一地址的最高优先级仍有多个实际目标：Configuration 错误，不靠最后声明者获胜。
- 同一个实体元素和字节的重复高优先级映射不算存储实体冲突，不生成 shadow warning。
- 选中实体后权限拒绝、resolver 或 callback 错误：结束本次访问，不回落到低优先级目标。

`map.layout_warnings()` 不执行设备代码，始终可查询 warning。`finish()` 使用已绑定的 hlog logger 在 WARN 输出；未绑定 logger 或 WARN 被过滤时不会凭空输出到 stderr。每个被覆盖实体与覆盖者/入口组合合并记录一次，包含双方实体/视图、首个冲突的 4-byte 解码槽地址、局部优先级路径及覆盖声明位置。shadow warning 不会改变快照和复位归属。

被遮挡的 VF 实体仍在存储中，可内部访问，也仍按自己的 scope 复位、按 SnapshotKind 保存；PF 共享实体不会因为挂在 VF View 内而变成 VF 实体。寄存器的显示地址只是诊断位置，运行时目标以实体引用和元素编号标识，不依赖某个可被遮挡的物理地址。

## 高地址与运行时

`reg::space()` 默认使用 `[0, UINT64_MAX & ~uint64_t{3})`，这是 uint64 可表示的最大 4-byte 对齐半开长度，不包含地址空间最后 4 个字节。`space(bytes)` 可继续显式限制范围。REG/View 的偏移、size、stride 均保持原有非零/对齐/边界要求；地址常量使用 ULL，不能先存入 uint32_t 再传入。

≤4 GiB 范围保留直接页目录；更大范围使用按需分配的页索引。没有寄存器的巨大间隔不分配页。IdentityResolveRequest 的 bus_offset、canonical_offset、offset_in_instance 均为 uint64_t，身份编号仍为 uint32_t。

构建时解析布局与优先级，运行时直接查找可见实体记录；不遍历 View 树、不排序优先级，也不在运行时追踪静态 alias 链。动态 VF/index resolver 仍按协议执行。一个访问跨越两个不同可见目标时会被拒绝，不自动拆成两次业务访问。布局固定后才能运行；当前版本没有动态挂载/改优先级、热插拔或自动多线程同步。

`describe_layout()` 仅展示 View/Entity 布局；Full 才增加紧凑寄存器地址表。`explain_address()` 展示选中的 View、局部优先级、实体和身份规则，不执行 resolver。运行时日志继续单行 Trace，info.address 保留来源入口地址。兼容字段 alias_resolved_address/resolved_address 使用实体代表地址，但选择目标不再通过该地址二次查找；有 shadow 时不要把代表地址当作实体唯一身份。

## 完整可运行程序

源文件：[address_views.cpp](snippets/address_views.cpp)。布局使用你给出的两个基地址，各有 8 个 2 MiB VF 入口。

<!-- GENERATED ADDRESS VIEW EXAMPLE BEGIN -->
```cpp
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>

constexpr std::uint64_t BASE_A = 0xff71f0000000ULL;
constexpr std::uint64_t BASE_B = 0xc100000000ULL;
constexpr std::uint64_t VF_STRIDE = 0x200000;
constexpr unsigned VF_COUNT = 8;
REG_ACCESS_FIELD(VfRoute, std::uint32_t);

struct PfRegs : reg::RegEntityBundle {
    REG_BUNDLE(PfRegs);
    REG(STATUS, std::uint32_t, reg::at(0x40).reset(7));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0).reset(0));
    REG(COUNTER, std::uint32_t, reg::at(0x80).reset(9));
    // +0x40 is a planned hole, so overlaying PF.STATUS produces no warning.
};
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_ENTITY(pf, PfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, VfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));

    REG_LAYOUT([&] {
        const auto pf_shared = reg::view("PF_SHARED", VF_STRIDE).map(pf);
        auto vf_view = [&](auto i) {
            return reg::view("VF", VF_STRIDE)
                .identity(1)
                .using_resolver(VfRoute)
                .map(vf[i])
                .map(pf_shared, 0, 10);
        };
        return reg::address_space("DEVICE")
            .map_repeat("ENTRY_A", BASE_A, reg::repeat(VF_COUNT).stride(VF_STRIDE), vf_view)
            .map_repeat("ENTRY_B", BASE_B, reg::repeat(VF_COUNT).stride(VF_STRIDE), vf_view);
    });
};
int main() {
    reg::RegisterBank<Layout> bank(reg::space().name("DEVICE"));
    bank.map().set_identity_resolver(VfRoute, [](const reg::IdentityResolveRequest& r) {
        return r.request_id;
    });
    bank.finish();
    auto& map = bank.map();
    if (!map.mmio_write(BASE_A + 3 * VF_STRIDE, 42).success() ||
        map.mmio_read(BASE_B + 3 * VF_STRIDE) != 42)
        throw std::runtime_error("VF aliases do not share storage");
    if (!map.mmio_write(BASE_B + VF_STRIDE + 0x40, 99).success() ||
        map.mmio_read(BASE_A + 7 * VF_STRIDE + 0x40) != 99)
        throw std::runtime_error("PF shared storage failed");
    if (map.mmio_read(BASE_B + VF_STRIDE + 0x80) != 9 || !map.layout_warnings().empty())
        throw std::runtime_error("transparent overlay holes failed");
    std::cout
        << "PASS: two high-address entrances, VF private/PF shared storage, transparent holes\n";
}
```
<!-- GENERATED ADDRESS VIEW EXAMPLE END -->

## 将 POE 的八块共享寄存器变成一个可复用 View

保留现有 vcpu_int/lcpu_cfg/queue_cfg/grp_ovfl/grp_order/grp_stats/queue_pkt/vcpu_stats 八个 REG_ENTITY；不必修改它们内部的 REG 声明，也不必强行合并存储类型。

```cpp
const auto pf_shared = reg::view("PF_SHARED", POE_BUSINESS_SIZE)
    .map(vcpu_int,   SHARED_VCPU_INT_OFF)
    .map(lcpu_cfg,   SHARED_LCPU_CFG_OFF)
    .map(queue_cfg,  SHARED_QUEUE_CFG_OFF)
    .map(grp_ovfl,   SHARED_GRP_OVFL_OFF)
    .map(grp_order,  SHARED_GRP_ORDER_OFF)
    .map(grp_stats,  SHARED_GRP_STATS_OFF)
    .map(queue_pkt,  SHARED_QUEUE_PKT_OFF)
    .map(vcpu_stats, SHARED_VCPU_STATS_OFF);

auto business = [&](auto i) {
    return reg::view("BUSINESS", POE_BUSINESS_SIZE)
        .map(vf[i])
        .map(pf_shared, 0, 10);
};

const auto module_ro = reg::view("MODULE_RO", POE_MODULE_SIZE)
    .map(pf_module).read_only_ignore_writes();
auto vf_view = [&](auto i) {
    return reg::view("VF_WINDOW", POE_VF_WINDOW_SIZE)
        .identity(POE_VF_DOMAIN).using_resolver(VfRoute)
        .vf_to_pf()
        .map(module_ro, 0)
        .map(business(i), POE_MODULE_SIZE)
        .map(cmd, POE_COMMAND_OFFSET);
};

return reg::address_space("POE_CPU")
    .map_repeat("VF_ENTRY_A", 0xff71f0000000ULL,
        reg::repeat(POE_VF_COUNT).stride(POE_VF_WINDOW_STRIDE), vf_view)
    .map_repeat("VF_ENTRY_B", 0xc100000000ULL,
        reg::repeat(POE_VF_COUNT).stride(POE_VF_WINDOW_STRIDE), vf_view);
// 需要保留 PF 地址入口时，再对同一个 AddressSpace .map(pf_view, PF_BASE)。
```

原来每块共享实体内的偏移从其块内 0 起算，所以 shared view 的 .map(...,SHARED_OFF) 正好加一次业务偏移。PoeVfRegs 留出的空洞与共享 REG 互补时没有 warning；如果误写了同地址 VF REG，PF 高优先级映射生效并告警。

原代码若把 vf[POE_VF_COUNT] 当成 PF 业务实例，同时配置 reset_per_instance(Vf)，该额外实例实际上得到 Vf(8) scope。需要 PF reset 语义时应单独声明 PF scope 的 pf_business，再以同样的 View 组合它。

## 兼容边界

旧 Window DSL 已删除。新 AddressSpace 根要求存储通过 REG_ENTITY/REG_REPEAT_ENTITY 创建；不能混用旧式有地址存储或在构建后追加旧窗口。一个布局路径仍只有一份 VF identity；HAC 等额外逻辑字段可在设备入口/访问组中处理，本版没有隐式引入多层身份转换。

快照格式仍为 v2：只保存实体与活跃事务，不保存 View 描述/分发表；加载前先重建同一设备布局并绑定回调。使用新旧两种布局，只要实体快照 schema 兼容，就不因增加地址别名而复制状态。完整约束见 [Snapshot](Snapshot.md)。

## 完整 PF/VF 布局声明

<!-- GENERATED POE VIEWS BEGIN -->
```cpp
struct DeviceRegs : reg::Bundle {
    REG_BUNDLE(DeviceRegs);
    REG_ENTITY(module, ModuleRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_ENTITY(pf, PfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf, VfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf));

    REG_ENTITY(pf_commands, CommandRegs, reg::entity().reset_scope({reg::ResetScope::Pf}));
    REG_REPEAT_ENTITY(vf_commands,
                      CommandRegs,
                      VF_COUNT,
                      reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_LAYOUT([&] {
        const auto module_view = reg::view("MODULE", HEAD_SIZE).map(module);
        auto business = [&](auto i) {
            return reg::view("BUSINESS", BUSINESS_SIZE)
                .map(vf[i])
                .map(pf.CPU_SHARED, 0x40, 10)
                .map(pf.GROUP_SHARED, 0x80, 10);
        };
        const auto pf_view = reg::view("PF", 16 * MiB)
                                 .map(module_view)
                                 .map(reg::view("PF_BUSINESS", BUSINESS_SIZE).map(pf), HEAD_SIZE)
                                 .map(reg::view("PF_COMMAND", MiB).map(pf_commands), MiB)
                                 .reserve(10 * MiB, 6 * MiB);
        auto poe_cpu =
            reg::view("DEVICE", SPACE_SIZE)
                .map(pf_view, 0)
                .map_repeat("PF_TO_VF",
                            2 * MiB,
                            reg::repeat(VF_COUNT).stride(MiB),
                            [&](auto i) {
                                return reg::view("SLOT", MiB)
                                    .identity(VF_DOMAIN)
                                    .using_resolver(VfRoute)
                                    .reserve(0, HEAD_SIZE)
                                    .map(business(i).pf_to_vf(), HEAD_SIZE);
                            })
                .map_repeat("VF", 16 * MiB, reg::repeat(VF_COUNT).stride(2 * MiB), [&](auto i) {
                    return reg::view("VF_WINDOW", 2 * MiB)
                        .identity(VF_DOMAIN)
                        .using_resolver(VfRoute)
                        .map(module_view.read_only_ignore_writes().vf_to_pf())
                        .map(business(i).vf_to_pf(), HEAD_SIZE)
                        .map(reg::view("VF_COMMAND", MiB).map(vf_commands[i]).vf_to_pf(), MiB);
                });
        return reg::address_space("POE").map(poe_cpu, 0);
    });
};
```
<!-- GENERATED POE VIEWS END -->

## 布局日志

`finish()` 在 **Debug** 输出 Summary：只列 View、Entity 映射、保留区和重复族的大范围、优先级、复位归属，不列各个 REG。**Trace** 另行输出 Full，寄存器地址详情真正使用 TRACE 严重等级。Info 不自动打印布局。

Full 对同一个映射下的数组 REG 只输出一行：`base + x0*stride0 + x1*stride1`，同时列 `dims`、`strides`、可见元素数量和线性选择范围。可见选择相同的重复 VF 只列一个代表和 `i=0..N-1`/stride；不会逐 VF、逐数组元素或逐 32-bit beat 展开。宽 REG 同样一行。

若只有部分数组元素或部分字节被覆盖，表中显示实际可见的元素、word 数量与 partial 标记。公式表达原声明几何，不代表空洞或被覆盖位置仍映射到该实体。过于碎片化的选择范围最多列八段，余段标数量；具体地址用 `explain_address(addr)` 查询。

`describe_layout()` / `describe_layout(LayoutDetail::Summary)` 返回上述概览，`describe_layout(LayoutDetail::Full)` 返回紧凑寄存器详情，均不执行设备 resolver 或回调。

紧凑寄存器地址表按每行代表的最小实际可见入口地址进行 64 位数值升序排序；数组仍只列一行。部分覆盖时不使用可能已隐藏的数组首地址作为排序依据。

PF 自己的业务存储应使用独立的 `REG_ENTITY(pf_business, PoeVfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}))`；VF 用 `REG_REPEAT_ENTITY(vf, PoeVfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf))`。不要为 PF 在 VF 重复数组后额外加一项：若 count 为 VF_COUNT+1，vf[VF_COUNT] 确实是独立实体，并会被标为 Vf(VF_COUNT)，打印它不表示硬件新增了一个 VF。PF 业务实体有独立 PF 入口，不能当作 VF 重复族的副本省略。

## 先组合完整局部布局，再挂载高地址入口（0.23.2）

下面是 REG_LAYOUT 内的布局片段，pf_view、pf_vf_slot(i)、vf_window(i) 为设备此前构造的 View/工厂：

```cpp
const auto poe_cpu = reg::view("POE_CPU", 32 * MiB)
    .map(pf_view, 0)
    .map_repeat("PF_VF", POE_PF_VF_BASE,
        reg::repeat(POE_VF_COUNT).stride(POE_PF_VF_STRIDE), pf_vf_slot)
    .map_repeat("VF", POE_VF_WINDOW_BASE,
        reg::repeat(POE_VF_COUNT).stride(POE_VF_WINDOW_STRIDE), vf_window);

return reg::address_space("DEVICE")
    .map(poe_cpu, 0xff71f0000000ULL)
    .map(poe_cpu, 0xc100000000ULL);
```

每个外层 .map 入口都映射整个 32 MiB View。最终地址为入口 base + View 内偏移 + 实例 i*stride + 寄存器局部偏移；父 View 的 size 是局部边界，不受两个物理入口间距影响。bank 使用 reg::space() 或足够大的显式上界；MMIO 输入与布局使用同一地址坐标。

重复族在每个挂载入口内分别解析 VF 身份，两个入口仍指向同一实体实例。嵌套容器不改变实体 reset/save/load 归属，也不复制事务。路径上仍只支持一层 identity 转换，不能同时在父、子 View 各挂一套 VF identity。

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

View pre_access 的测量方法、完整数据和适用边界见 [性能验证](Performance.md)。完整可运行命名钩子示例见 [view_pre_access.cpp](snippets/view_pre_access.cpp)。

0.24.1 优化了钩子调用：普通 MMIO 调用不复制函数的 shared_ptr；仅当回调在 pre_access 链执行期间重绑/清除时，保留旧函数至本次 pre_access 链结束。回调内自修改仍受支持，后续钩子读取最新绑定；旧捕获对象的析构时点可能延后到本次 pre_access 链结束。框架仍不支持未同步的并发重绑。
